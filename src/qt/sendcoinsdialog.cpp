// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <qt/sendcoinsdialog.h>
#include <qt/forms/ui_sendcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendcoinsentry.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <txmempool.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/types.h>
#include <wallet/wallet.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <tuple>

#include <QDebug>
#include <QFontMetrics>
#include <QScrollBar>
#include <qt/guipreferences.h>
#include <QScopeGuard>
#include <QSaveFile>
#include <QShowEvent>
#include <QTextDocument>

using common::PSBTError;
using wallet::CCoinControl;

static constexpr std::array confTargets{2, 4, 6, 12, 24, 48, 144, 504, 1008};
int getConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(confTargets.size())) {
        return confTargets.back();
    }
    if (index < 0) {
        return confTargets[0];
    }
    return confTargets[index];
}
int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < confTargets.size(); i++) {
        if (confTargets[i] >= target) {
            return i;
        }
    }
    return confTargets.size() - 1;
}

SendCoinsDialog::SendCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::SendCoinsDialog),
    m_coin_control(new CCoinControl),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->addButton->setIcon(QIcon());
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->addButton->setIcon(_platformStyle->SingleColorIcon(":/icons/add"));
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->sendButton->setIcon(_platformStyle->SingleColorIcon(":/icons/send"));
    }

    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    addEntry();

    connect(ui->addButton, &QPushButton::clicked, this, &SendCoinsDialog::addEntry);
    connect(ui->clearButton, &QPushButton::clicked, this, &SendCoinsDialog::clear);

    // Coin Control
    connect(ui->pushButtonCoinControl, &QPushButton::clicked, this, &SendCoinsDialog::coinControlButtonClicked);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 7, 0))
    connect(ui->checkBoxCoinControlChange, &QCheckBox::checkStateChanged, this, &SendCoinsDialog::coinControlChangeChecked);
#else
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged, this, &SendCoinsDialog::coinControlChangeChecked);
#endif
    connect(ui->lineEditCoinControlChange, &QValidatedLineEdit::textChanged, this, &SendCoinsDialog::coinControlChangeEdited);
    m_change_debounce.setSingleShot(true);
    m_change_debounce.setInterval(250);
    m_change_poll.setInterval(50);
    connect(&m_change_debounce, &QTimer::timeout, this, &SendCoinsDialog::startChangeCheck);
    connect(&m_change_poll, &QTimer::timeout, this, &SendCoinsDialog::pollChangeCheck);

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardBytes);
    connect(clipboardChangeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardChange);
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    // init transaction fee section
    GuiSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->customFee->SetAllowEmpty(false);
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    GUIUtil::ExceptionSafeConnect(ui->sendButton, &QPushButton::clicked, this, &SendCoinsDialog::sendButtonClicked);

    connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::updateSmartFeeLabel);
    connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::coinControlUpdateLabels);
    connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::updateFeeSectionControls);
    connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::coinControlUpdateLabels);
    connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
    m_fee_refresh_timer.setParent(this);
    m_fee_refresh_timer.setObjectName("sendFeeRefreshTimer");
    connect(&m_fee_refresh_timer, &QTimer::timeout, this, &SendCoinsDialog::pollFeeEstimate);
    m_fee_refresh_timer.setInterval(100);
    renderSmartFeeLabel();
}

void SendCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    if (clientModel) disconnect(clientModel, nullptr, this, nullptr);
    this->clientModel = _clientModel;
    m_fee_refresh_enabled = _clientModel != nullptr;
    ++m_fee_generation;
    m_fee_refresh_requested = false;
    m_fee_refresh_timer.stop();
    coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &SendCoinsDialog::updateNumberOfBlocks);
        connect(_clientModel, &QObject::destroyed, this, [this] { setClientModel(nullptr); });
        updateSmartFeeLabel();
    }
}

void SendCoinsDialog::setModel(WalletModel *_model)
{
    for (const auto& connection : m_model_connections) disconnect(connection);
    m_model_connections.clear();
    ++m_fee_generation;
    m_fee_refresh_requested = false;
    m_cached_fee_estimate.reset();
    m_fee_refresh_timer.stop();
    this->model = _model;
    coinControlChangeEdited(ui->lineEditCoinControlChange->text());
    renderSmartFeeLabel();

    if (!_model) {
        for (int i = 0; i < ui->entries->count(); ++i) {
            if (auto* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget())) entry->setModel(nullptr);
        }
        return;
    }
    m_model_connections.push_back(connect(_model, &QObject::destroyed, this, [this] { setModel(nullptr); }));

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        m_model_connections.push_back(connect(_model, &WalletModel::balanceChanged, this, &SendCoinsDialog::setBalance));
        m_model_connections.push_back(connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::refreshBalance));
        refreshBalance();

        // Coin Control
        m_model_connections.push_back(connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::coinControlUpdateLabels));
        m_model_connections.push_back(connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &SendCoinsDialog::coinControlFeatureChanged));
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        // fee section
        if (ui->confTargetSelector->count() == 0) {
            for (const int n : confTargets) {
                ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n*Params().GetConsensus().nPowTargetSpacing)).arg(n));
            }
        }
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->SetMinValue(requiredFee);
        if (ui->customFee->value() < requiredFee) {
            ui->customFee->setValue(requiredFee);
        }
        ui->customFee->setSingleStep(requiredFee);
        updateFeeSectionControls();

        if (model->wallet().hasExternalSigner()) {
            //: "device" usually means a hardware wallet.
            ui->sendButton->setText(tr("Sign on device"));
            if (model->getOptionsModel()->hasSigner()) {
                ui->sendButton->setEnabled(true);
                ui->sendButton->setToolTip(tr("Connect your hardware wallet first."));
            } else {
                ui->sendButton->setEnabled(false);
                //: "External signer" means using devices such as hardware wallets.
                ui->sendButton->setToolTip(tr("Set external signer script path in Options -> Wallet"));
            }
        } else if (model->wallet().privateKeysDisabled()) {
            ui->sendButton->setText(tr("Cr&eate Unsigned"));
            ui->sendButton->setToolTip(tr("Creates a Partially Signed ConnectCoin Transaction (PSBT) for use with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(CLIENT_NAME));
        }

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        GuiSettings settings;
        if (settings.value("nSmartFeeSliderPosition").toInt() != 0) {
            // migrate nSmartFeeSliderPosition to nConfTarget
            // nConfTarget is available since 0.15 (replaced nSmartFeeSliderPosition)
            int nConfirmTarget = 25 - settings.value("nSmartFeeSliderPosition").toInt(); // 25 == old slider range
            settings.setValue("nConfTarget", nConfirmTarget);
            settings.remove("nSmartFeeSliderPosition");
        }
        if (settings.value("nConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(model->wallet().getConfirmTarget()));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
        updateSmartFeeLabel();
        updateSendButton();
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    GuiSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());

    delete ui;
}

bool SendCoinsDialog::PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text)
{
    const QPointer<SendCoinsDialog> guard{this};
    const QPointer<WalletModel> bound_model{model};
    if (!model || m_change_pending) return false;
    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate(model->node()))
            {
                recipients.append(entry->getValue());
            }
            else if (valid)
            {
                ui->scrollArea->ensureWidgetVisible(entry);
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return false;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!guard || !bound_model || model != bound_model || m_change_pending) return false;
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return false;
    }

    // prepare transaction for getting txFee earlier
    auto transaction = std::make_unique<WalletModelTransaction>(recipients);
    WalletModel::SendCoinsReturn prepareStatus;

    updateCoinControlState();

    CCoinControl coin_control = *m_coin_control;
    coin_control.m_allow_other_inputs = !coin_control.HasSelected(); // future, could introduce a checkbox to customize this value.
    const auto change_generation = m_change_generation;
    prepareStatus = model->prepareTransaction(*transaction, coin_control);
    if (!guard || !bound_model || model != bound_model) return false;
    if (change_generation != m_change_generation || m_change_pending) return false;
    m_current_transaction = std::move(transaction);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), m_current_transaction->getTransactionFee()));
    if (!guard || !bound_model || model != bound_model) return false;

    if(prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
        return false;
    }

    CAmount txFee = m_current_transaction->getTransactionFee();
    // A large recipient list needs one backend snapshot, not one responsive
    // wallet-enumeration wait per recipient. Formatting copied value data also
    // needs no widgets, so keep the entire recipient-sized loop on the worker.
    const auto [recipient_count, recipient_text, recipient_total] = GUIUtil::WaitForBackendTask(model->clientModel().requestNodeData(
        [recipients = m_current_transaction->getRecipients(), wallet_name = model->getWalletName(),
         unit = model->getOptionsModel()->getDisplayUnit()](interfaces::Node& node) {
            const bool multiwallet = node.walletLoader().getWallets().size() > 1;
            const auto escaped_name = GUIUtil::HtmlEscape(wallet_name);
            QStringList result;
            CAmount total{0};
            result.reserve(recipients.size());
            for (const auto& recipient : recipients) {
                total += recipient.amount;
                QString amount = BitcoinUnits::formatWithUnit(unit, recipient.amount);
                if (multiwallet) amount = tr("%1 from wallet '%2'").arg(amount, escaped_name);
                if (!recipient.label.isEmpty()) {
                    result.append(tr("%1 to '%2'").arg(amount, GUIUtil::HtmlEscape(recipient.label)) +
                                  QString(" (%1)").arg(recipient.address));
                } else {
                    result.append(tr("%1 to %2").arg(amount, recipient.address));
                }
            }
            return std::tuple{result.size(), result.size() == 1 ? result.front() : result.join("\n\n"), total};
        }), this);
    if (!guard || !bound_model || model != bound_model) return false;
    if (change_generation != m_change_generation || m_change_pending) return false;

    /*: Message displayed when attempting to create a transaction. Cautionary text to prompt the user to verify
        that the displayed transaction details represent the transaction the user intends to create. */
    question_string.append(tr("Do you want to create this transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    if (model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can only create a PSBT. This string is displayed when private keys are disabled and an external
            signer is not available. */
        question_string.append(tr("Please, review your transaction proposal. This will produce a Partially Signed ConnectCoin Transaction (PSBT) which you can save or copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(CLIENT_NAME));
    } else if (model->getOptionsModel()->getEnablePSBTControls()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can send their transaction or create a PSBT. This string is displayed when both private keys
            and PSBT controls are enabled. */
        question_string.append(tr("Please, review your transaction. You can create and send this transaction or create a Partially Signed ConnectCoin Transaction (PSBT), which you can save or copy and then sign with, e.g., an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(CLIENT_NAME));
    } else {
        /*: Text to prompt a user to review the details of the transaction they are attempting to send. */
        question_string.append(tr("Please, review your transaction."));
    }
    question_string.append("</span>%1");

    if(txFee > 0)
    {
        // append fee string if a fee is required
        question_string.append("<hr /><b>");
        question_string.append(tr("Transaction fee"));
        question_string.append("</b>");

        // append transaction size
        //: When reviewing a newly created PSBT (via Send flow), the transaction fee is shown, with "virtual size" of the transaction displayed for context
        question_string.append(" (" + tr("%1 kvB", "PSBT transaction creation").arg((double)m_current_transaction->getTransactionSize() / 1000, 0, 'g', 3) + "): ");

        // append transaction fee value
        question_string.append("<span style='color:#aa0000; font-weight:bold;'>");
        question_string.append(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        question_string.append("</span><br />");
    }

    // append RBF message
    question_string.append("<span style='font-size:10pt; font-weight:normal;'>");
    question_string.append(tr("You can increase the fee later."));

    // add total amount in all subdivision units
    question_string.append("<hr />");
    CAmount totalAmount = recipient_total + txFee;
    QStringList alternativeUnits;
    for (const BitcoinUnit u : BitcoinUnits::availableUnits()) {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
    }
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
        .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeUnits.join(" " + tr("or") + " ")));

    if (recipient_count > 1) {
        question_string = question_string.arg("");
        informative_text = tr("To review recipient list click \"Show Details…\"");
        detailed_text = recipient_text;
    } else {
        question_string = question_string.arg("<br /><br />" + recipient_text);
    }

    return true;
}

void SendCoinsDialog::presentPSBT(PartiallySignedTransaction psbtx, const QList<SendCoinsRecipient>& recipients)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<SendCoinsDialog> guard{this};
    const QPointer<WalletModel> bound_model{model};
    if (!model) return;
    auto serialized = GUIUtil::WaitForBackendTask(model->requestWalletData([psbt = std::move(psbtx)](interfaces::Wallet&) {
        DataStream stream{};
        stream << psbt;
        auto binary = stream.str();
        auto base64 = QString::fromStdString(EncodeBase64(binary));
        return std::pair{std::move(binary), std::move(base64)};
    }), this);
    if (!guard || !bound_model || model != bound_model) return;
    GUIUtil::setClipboard(serialized.second);
    auto* msgBox = new QMessageBox(this);
    msgBox->setAttribute(Qt::WA_DeleteOnClose);
    //: Caption of "PSBT has been copied" messagebox
    msgBox->setText(tr("Unsigned Transaction", "PSBT copied"));
    msgBox->setInformativeText(tr("The PSBT has been copied to the clipboard. You can also save it."));
    msgBox->setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    msgBox->setDefaultButton(QMessageBox::Discard);
    msgBox->setObjectName("psbt_copied_message");
    const auto answer = msgBox->exec();
    if (!guard || !bound_model || model != bound_model) return;
    switch (answer) {
    case QMessageBox::Save: {
        QString selectedFilter;
        if (!guard || !bound_model || model != bound_model) return;
        const auto fileNameSuggestion = GUIUtil::WaitForBackendTask(model->requestWalletData(
            [recipients, unit = model->getOptionsModel()->getDisplayUnit()](interfaces::Wallet&) {
                QStringList parts;
                parts.reserve(recipients.size());
                for (const auto& recipient : recipients) {
                    parts.append((recipient.label.isEmpty() ? recipient.address : recipient.label) + "-" +
                                 BitcoinUnits::formatWithUnit(unit, recipient.amount));
                }
                return parts.join(" - ") + ".psbt";
            }), this);
        if (!guard || !bound_model || model != bound_model) return;
        QString filename = GUIUtil::getSaveFileName(this,
            tr("Save Transaction Data"), fileNameSuggestion,
            //: Expanded name of the binary PSBT file format. See: BIP 174.
            tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selectedFilter);
        if (!guard || !bound_model || model != bound_model || filename.isEmpty()) {
            return;
        }
        const auto error = GUIUtil::WaitForBackendTask(model->requestWalletData([filename, binary = std::move(serialized.first)](interfaces::Wallet&) -> QString {
            QSaveFile file(filename);
            if (!file.open(QIODevice::WriteOnly) || file.write(binary.data(), binary.size()) != static_cast<qint64>(binary.size()) || !file.commit()) {
                return file.errorString();
            }
            return {};
        }), this);
        if (!guard) return;
        if (!error.isEmpty()) {
            Q_EMIT message(tr("Save failed"), error, CClientUIInterface::MSG_ERROR);
            return;
        }
        //: Popup message when a PSBT has been saved to a file
        Q_EMIT message(tr("PSBT saved"), tr("PSBT saved to disk"), CClientUIInterface::MSG_INFORMATION);
        break;
    }
    case QMessageBox::Discard:
        break;
    default:
        assert(false);
    } // msgBox.exec()
}

bool SendCoinsDialog::signWithExternalSigner(PartiallySignedTransaction& psbtx, WalletModelTransaction& transaction, bool& complete) {
    GUIUtil::BackendOperationGuard operation;
    const QPointer<SendCoinsDialog> guard{this};
    const QPointer<WalletModel> bound_model{model};
    if (!model) return false;
    std::optional<PSBTError> err;
    try {
        struct SignedPSBT {
            PartiallySignedTransaction psbt;
            std::unique_ptr<WalletModelTransaction> transaction;
            bool complete{false};
            std::optional<PSBTError> error;
        };
        auto result = GUIUtil::WaitForBackendTask(model->requestWalletData([psbt = std::move(psbtx), transaction](interfaces::Wallet& wallet) mutable {
            SignedPSBT result{std::move(psbt), std::make_unique<WalletModelTransaction>(std::move(transaction)), false, {}};
            result.error = wallet.fillPSBT({.sign = true, .bip32_derivs = true}, /*n_signed=*/nullptr, result.psbt, result.complete);
            // fillPSBT does not always finalize an external signature.
            if (!result.error) {
                CMutableTransaction finalized;
                result.complete = FinalizeAndExtractPSBT(result.psbt, finalized);
                if (result.complete) result.transaction->setWtx(MakeTransactionRef(std::move(finalized)));
            }
            return result;
        }), this);
        if (!guard || !bound_model || model != bound_model) return false;
        psbtx = std::move(result.psbt);
        if (result.complete) transaction = std::move(*result.transaction);
        complete = result.complete;
        err = result.error;
    } catch (const std::runtime_error& e) {
        if (!guard) return false;
        QMessageBox::critical(nullptr, tr("Sign failed"), e.what());
        return false;
    }
    if (err == PSBTError::EXTERNAL_SIGNER_NOT_FOUND) {
        //: "External signer" means using devices such as hardware wallets.
        const QString msg = tr("External signer not found");
        QMessageBox::critical(nullptr, msg, msg);
        return false;
    }
    if (err == PSBTError::EXTERNAL_SIGNER_FAILED) {
        //: "External signer" means using devices such as hardware wallets.
        const QString msg = tr("External signer failure");
        QMessageBox::critical(nullptr, msg, msg);
        return false;
    }
    if (err) {
        qWarning() << "Failed to sign PSBT";
        processSendCoinsReturn(WalletModel::TransactionCreationFailed);
        return false;
    }
    return true;
}

void SendCoinsDialog::sendButtonClicked([[maybe_unused]] bool checked)
{
    GUIUtil::BackendOperationGuard operation;
    if (!model || !model->getOptionsModel() || m_change_pending || m_send_in_progress) return;
    const QPointer<SendCoinsDialog> guard{this};
    const QPointer<WalletModel> bound_model{model};
    m_send_in_progress = true;
    updateSendButton();
    const auto finish_action = qScopeGuard([guard] {
        if (!guard) return;
        guard->m_send_in_progress = false;
        guard->fNewRecipientAllowed = true;
        guard->updateSendButton();
    });

    QString question_string, informative_text, detailed_text;
    if (!PrepareSendText(question_string, informative_text, detailed_text)) return;
    if (!guard || !bound_model || model != bound_model) return;
    assert(m_current_transaction);
    // Keep the reviewed transaction alive independently of the page while
    // confirmations and backend waits process GUI events.
    auto transaction = std::move(m_current_transaction);
    const auto recipients = transaction->getRecipients();
    const auto reviewed_change_generation = m_change_generation;

    const QString confirmation = tr("Confirm send coins");
    const bool enable_send{!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner()};
    const bool always_show_unsigned{model->getOptionsModel()->getEnablePSBTControls()};
    auto confirmationDialog = new SendConfirmationDialog(confirmation, question_string, informative_text, detailed_text, SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, this);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());
    if (!guard || !bound_model || model != bound_model) return;
    if (reviewed_change_generation != m_change_generation || m_change_pending) return;

    if(retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
        fNewRecipientAllowed = true;
        return;
    }

    bool send_failure = false;
    struct FilledPSBT {
        PartiallySignedTransaction psbt;
        bool complete{false};
        std::optional<PSBTError> error;
    };
    const auto fill_unsigned = [this](const CTransactionRef& tx) {
        return GUIUtil::WaitForBackendTask(model->requestWalletData([tx](interfaces::Wallet& wallet) {
            FilledPSBT result{PartiallySignedTransaction{CMutableTransaction{*tx}}, false, {}};
            result.error = wallet.fillPSBT({.sign = false, .bip32_derivs = true}, /*n_signed=*/nullptr, result.psbt, result.complete);
            return result;
        }), this);
    };
    if (retval == QMessageBox::Save) {
        // "Create Unsigned" clicked
        auto filled = fill_unsigned(transaction->getWtx());
        if (!guard || !bound_model || model != bound_model) return;
        if (filled.error) {
            processSendCoinsReturn(WalletModel::TransactionCreationFailed);
            return;
        }

        // Copy PSBT to clipboard and offer to save
        presentPSBT(std::move(filled.psbt), recipients);
    } else {
        // "Send" clicked
        assert(!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner());
        bool broadcast = true;
        if (model->wallet().hasExternalSigner()) {
            // Always fill without signing first. This prevents an external signer
            // from being called before the user has approved the transaction.
            auto filled = fill_unsigned(transaction->getWtx());
            if (!guard || !bound_model || model != bound_model) return;
            if (filled.error) {
                processSendCoinsReturn(WalletModel::TransactionCreationFailed);
                return;
            }
            send_failure = !signWithExternalSigner(filled.psbt, *transaction, filled.complete);
            if (!guard || !bound_model || model != bound_model) return;
            // Don't broadcast when user rejects it on the device or there's a failure:
            broadcast = filled.complete && !send_failure;
            if (!send_failure) {
                // A transaction signed with an external signer is not always complete,
                // e.g. in a multisig wallet.
                if (!filled.complete) {
                    presentPSBT(std::move(filled.psbt), recipients);
                    if (!guard || !bound_model || model != bound_model) return;
                }
            }
        }

        // Broadcast the transaction, unless an external signer was used and it
        // failed, or more signatures are needed.
        if (broadcast) {
            // now send the prepared transaction
            const auto txid = transaction->getWtx()->GetHash();
            model->sendCoins(*transaction);
            if (!guard || !bound_model || model != bound_model) return;
            Q_EMIT coinsSent(txid);
        }
    }
    if (!guard || !bound_model || model != bound_model) return;
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        coinControlUpdateLabels();
    }
    fNewRecipientAllowed = true;
}

void SendCoinsDialog::clear()
{
    m_current_transaction.reset();

    // Clear coin control settings
    m_coin_control->UnSelectAll();
    ui->checkBoxCoinControlChange->setChecked(false);
    ui->lineEditCoinControlChange->clear();
    coinControlUpdateLabels();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry *entry = new SendCoinsEntry(platformStyle, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, &SendCoinsEntry::removeEntry, this, &SendCoinsDialog::removeEntry);
    connect(entry, &SendCoinsEntry::useAvailableBalance, this, &SendCoinsDialog::useAvailableBalance);
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
    connect(entry, &SendCoinsEntry::subtractFeeFromAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());

    // Scroll to the newly added entry on a QueuedConnection because Qt doesn't
    // adjust the scroll area and scrollbar immediately when the widget is added.
    // Invoking on a DirectConnection will only scroll to the second-to-last entry.
    QMetaObject::invokeMethod(ui->scrollArea, [this] {
        if (ui->scrollArea->verticalScrollBar()) {
            ui->scrollArea->verticalScrollBar()->setValue(ui->scrollArea->verticalScrollBar()->maximum());
        }
    }, Qt::QueuedConnection);

    updateTabsAndLabels();
    return entry;
}

void SendCoinsDialog::updateTabsAndLabels()
{
    setupTabChain(nullptr);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString &address)
{
    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        CAmount balance = balances.balance;
        if (model->wallet().hasExternalSigner()) {
            ui->labelBalanceName->setText(tr("External balance:"));
        }
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
    }
}

void SendCoinsDialog::refreshBalance()
{
    if (!model || !model->getOptionsModel()) return;
    setBalance(model->getCachedBalance());
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    renderSmartFeeLabel();
}

void SendCoinsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // All status values are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getDefaultMaxTxFee()));
        break;
    case WalletModel::OK:
        return;
    } // no default case, so the compiler can warn about missing cases
    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void SendCoinsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void SendCoinsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void SendCoinsDialog::useAvailableBalance(SendCoinsEntry* entry)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<SendCoinsDialog> guard{this};
    const QPointer<SendCoinsEntry> target{entry};
    const QPointer<WalletModel> bound_model{model};
    if (!model || !entry) return;
    // Same behavior as send: if we have selected coins, only obtain their available balance.
    // Copy to avoid modifying the member's data.
    CCoinControl coin_control = *m_coin_control;
    coin_control.m_allow_other_inputs = !coin_control.HasSelected();

    // Calculate available amount to send.
    CAmount amount = model->getAvailableBalance(&coin_control);
    if (!guard || !target || !bound_model || model != bound_model) return;
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* e = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (e && !e->isHidden() && e != entry) {
            amount -= e->getValue().amount;
        }
    }

    if (amount > 0) {
      entry->checkSubtractFeeFromAmount();
      entry->setAmount(amount);
    } else {
      entry->setAmount(0);
    }
}

void SendCoinsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelCustomFeeWarning   ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked());
}

void SendCoinsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        ui->labelFeeMinimized->setText(tr("%1/kvB").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value())));
    }
}

void SendCoinsDialog::updateCoinControlState()
{
    if (ui->radioCustomFee->isChecked()) {
        m_coin_control->m_feerate = CFeeRate(ui->customFee->value());
    } else {
        m_coin_control->m_feerate.reset();
    }
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    m_coin_control->m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
}

void SendCoinsDialog::updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state) {
    // During shutdown, clientModel will be nullptr. Attempting to update views at this point may cause a crash
    // due to accessing backend models that might no longer exist.
    if (!clientModel) return;
    // Process event
    if (sync_state == SynchronizationState::POST_INIT) {
        m_fee_block_refresh_pending = true;
        refreshVisibleBlockFee();
    }
}

void SendCoinsDialog::refreshVisibleBlockFee()
{
    // Hidden wallet pages and minimized windows need no passive fee display
    // refresh on every block. Actual transaction preparation calculates its
    // fee independently; resume one coalesced display update when visible.
    if (m_fee_block_refresh_pending && isVisible() && !window()->isMinimized()) updateSmartFeeLabel();
}

void SendCoinsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    auto* top_level = window();
    if (m_watched_window != top_level) {
        if (m_watched_window && m_watched_window != this) m_watched_window->removeEventFilter(this);
        m_watched_window = top_level;
        if (top_level != this) top_level->installEventFilter(this);
    }
    refreshVisibleBlockFee();
}

void SendCoinsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) refreshVisibleBlockFee();
}

bool SendCoinsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_watched_window && event->type() == QEvent::WindowStateChange) refreshVisibleBlockFee();
    return QDialog::eventFilter(watched, event);
}

void SendCoinsDialog::updateSmartFeeLabel()
{
    if (!model || !model->getOptionsModel() || !m_fee_refresh_enabled)
        return;

    ++m_fee_generation;
    m_fee_block_refresh_pending = false;
    m_fee_refresh_requested = true;
    renderSmartFeeLabel();
    if (!m_fee_estimate.valid()) startFeeEstimate();
    m_fee_refresh_timer.start();
}

void SendCoinsDialog::startFeeEstimate()
{
    if (!model || !model->getOptionsModel() || !m_fee_refresh_enabled || m_fee_estimate.valid()) return;

    updateCoinControlState();
    // GetMinimumFeeRate only needs these fee policy inputs when estimating
    // without an explicit rate. Do not copy all selected UTXOs, scripts and
    // external signing data merely to display a fee for a fixed 1000 bytes.
    const auto target = m_coin_control->m_confirm_target;
    const auto signal_rbf = m_coin_control->m_signal_bip125_rbf;
    const auto fee_mode = m_coin_control->m_fee_mode;
    m_pending_fee_generation = m_fee_generation;
    m_fee_refresh_requested = false;
    try {
        m_fee_estimate = model->requestWalletData([target, signal_rbf, fee_mode](interfaces::Wallet& wallet) {
            CCoinControl control;
            control.m_confirm_target = target;
            control.m_signal_bip125_rbf = signal_rbf;
            control.m_fee_mode = fee_mode;
            FeeEstimate result{0, std::nullopt, FeeReason::REQUIRED, static_cast<int>(*target)};
            result.fee = wallet.getMinimumFee(1000, control, &result.returned_target, &result.reason);
            return result;
        });
    } catch (const std::exception& error) {
        qWarning() << "Unable to request fee estimate:" << error.what();
    }
}

void SendCoinsDialog::pollFeeEstimate()
{
    if (m_fee_estimate.valid()) {
        if (m_fee_estimate.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            const auto result = m_fee_estimate.get();
            if (model && m_fee_refresh_enabled && m_pending_fee_generation == m_fee_generation) {
                m_cached_fee_estimate = result;
                renderSmartFeeLabel();
            }
        } catch (const std::exception& error) {
            // Retain the last successful estimate. Transaction preparation
            // always calculates its own fee and never uses this display cache.
            qWarning() << "Unable to refresh fee estimate:" << error.what();
        }
    }
    if (m_fee_refresh_requested) startFeeEstimate();
    if (!m_fee_estimate.valid()) m_fee_refresh_timer.stop();
}

void SendCoinsDialog::renderSmartFeeLabel()
{
    if (!model || !model->getOptionsModel() || !m_cached_fee_estimate ||
        m_cached_fee_estimate->requested_target != getConfTargetForIndex(ui->confTargetSelector->currentIndex())) {
        ui->labelSmartFee->setText(QString::fromUtf8("\xe2\x80\xa6"));
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->clear();
        ui->fallbackFeeWarningLabel->hide();
        updateFeeMinimizedLabel();
        return;
    }
    const auto& estimate = *m_cached_fee_estimate;
    const auto& returned_target = estimate.returned_target;
    const auto reason = estimate.reason;

    ui->labelSmartFee->setText(tr("%1/kvB").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), estimate.fee)));

    if (reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
        ui->fallbackFeeWarningLabel->setIndent(GUIUtil::TextWidth(QFontMetrics(ui->fallbackFeeWarningLabel->font()), "x"));
    }
    else
    {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText("");
        if (returned_target) {
            ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", *returned_target));
        } else if (reason == FeeReason::REQUIRED || reason == FeeReason::MEMPOOL_MIN) {
            ui->labelFeeEstimation->setText(tr("Using the current minimum fee. Confirmation time is not estimated."));
        }
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) { // coin control features disabled
        m_coin_control = std::make_unique<CCoinControl>();
    }
    coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    auto dlg = new CoinControlDialog(*m_coin_control, model, platformStyle, this);
    connect(dlg, &QDialog::finished, this, &SendCoinsDialog::coinControlUpdateLabels);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

// Coin Control: checkbox custom change address
#if (QT_VERSION >= QT_VERSION_CHECK(6, 7, 0))
void SendCoinsDialog::coinControlChangeChecked(Qt::CheckState state)
#else
void SendCoinsDialog::coinControlChangeChecked(int state)
#endif
{
    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
    coinControlChangeEdited(ui->lineEditCoinControlChange->text());
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    ++m_change_generation;
    m_change_requested = false;
    m_change_pending = false;
    m_change_debounce.stop();
    m_change_poll.stop();
    m_coin_control->destChange = CNoDestination();
    if (m_change_confirmation) {
        m_change_confirmation->reject();
        m_change_confirmation = nullptr;
    }
    ui->labelCoinControlChangeLabel->clear();
    ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");
    if (!model || !model->getOptionsModel() || !m_fee_refresh_enabled || !ui->checkBoxCoinControlChange->isChecked() ||
        !model->getOptionsModel()->getCoinControlFeatures() || text.isEmpty()) {
        updateSendButton();
        return;
    }
    if (!IsValidDestination(DecodeDestination(text.toStdString()))) {
        ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid ConnectCoin address"));
        updateSendButton();
        return;
    }
    m_change_pending = true;
    m_change_requested = true;
    ui->labelCoinControlChangeLabel->setText(tr("Checking change address…"));
    updateSendButton();
    m_change_debounce.start();
}

void SendCoinsDialog::updateSendButton()
{
    const bool signer_available = model && model->getOptionsModel() &&
        (!model->wallet().hasExternalSigner() || model->getOptionsModel()->hasSigner());
    ui->sendButton->setEnabled(signer_available && m_fee_refresh_enabled && !m_change_pending && !m_send_in_progress);
}

void SendCoinsDialog::startChangeCheck()
{
    if (!model || !m_change_requested || !m_change_pending || !m_fee_refresh_enabled) return;
    if (m_change_query.valid()) {
        m_change_poll.start();
        return;
    }
    m_pending_change_generation = m_change_generation;
    m_pending_change_text = ui->lineEditCoinControlChange->text();
    m_pending_change_model = model;
    m_change_requested = false;
    const auto destination = DecodeDestination(m_pending_change_text.toStdString());
    try {
        m_change_query = model->requestWalletData([destination](interfaces::Wallet& wallet) { return wallet.isSpendable(destination); });
        m_change_poll.start();
    } catch (const std::exception& error) {
        qWarning() << "Change address verification failed:" << error.what();
        ui->labelCoinControlChangeLabel->setText(tr("Unable to verify change address. Edit the address to retry."));
        // Keep Send disabled until the address is cleared or verified.
    }
}

void SendCoinsDialog::pollChangeCheck()
{
    if (!m_change_query.valid() || m_change_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
    m_change_poll.stop();
    std::optional<bool> spendable;
    try {
        spendable = m_change_query.get();
    } catch (const std::exception& error) {
        qWarning() << "Change address verification failed:" << error.what();
    }
    const bool current = model && model == m_pending_change_model && m_fee_refresh_enabled &&
        m_pending_change_generation == m_change_generation && ui->checkBoxCoinControlChange->isChecked() &&
        ui->lineEditCoinControlChange->text() == m_pending_change_text && model->getOptionsModel()->getCoinControlFeatures();
    if (current) {
        if (!spendable) {
            ui->labelCoinControlChangeLabel->setText(tr("Unable to verify change address. Edit the address to retry."));
            return;
        }
        const QString text = m_pending_change_text;
        const auto destination = DecodeDestination(text.toStdString());
        if (*spendable) {
            m_coin_control->destChange = destination;
            m_change_pending = false;
            ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
            const auto label = model->getAddressTableModel()->labelForAddress(text);
            ui->labelCoinControlChangeLabel->setText(label.isEmpty() ? tr("(no label)") : label);
            updateSendButton();
            coinControlUpdateLabels();
        } else {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));
            auto* confirmation = new QMessageBox(QMessageBox::Question, tr("Confirm custom change address"),
                tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                QMessageBox::Yes | QMessageBox::Cancel, this);
            confirmation->setDefaultButton(QMessageBox::Cancel);
            m_change_confirmation = confirmation;
            connect(confirmation, &QDialog::finished, this,
                [this, text, destination, generation = m_change_generation, bound_model = QPointer<WalletModel>{model}](int result) {
                    if (!bound_model || model != bound_model || generation != m_change_generation ||
                        !ui->checkBoxCoinControlChange->isChecked() || ui->lineEditCoinControlChange->text() != text ||
                        !model->getOptionsModel()->getCoinControlFeatures()) return;
                    m_change_confirmation = nullptr;
                    m_change_pending = false;
                    if (result == QMessageBox::Yes) m_coin_control->destChange = destination;
                    else ui->lineEditCoinControlChange->clear();
                    updateSendButton();
                    coinControlUpdateLabels();
                });
            GUIUtil::ShowModalDialogAsynchronously(confirmation);
        }
    }
    if (m_change_requested && !m_change_debounce.isActive()) startChangeCheck();
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState();

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendCoinsRecipient rcp = entry->getValue();
            CoinControlDialog::payAmounts.append(rcp.amount);
            if (rcp.fSubtractFeeFromAmount)
                CoinControlDialog::fSubtractFeeFromAmount = true;
        }
    }

    if (m_coin_control->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(*m_coin_control, model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

SendConfirmationDialog::SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text, const QString& detailed_text, int _secDelay, bool enable_send, bool always_show_unsigned, QWidget* parent)
    : QMessageBox(parent), secDelay(_secDelay), m_enable_send(enable_send)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setDetailedText(detailed_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    if (always_show_unsigned || !enable_send) addButton(QMessageBox::Save);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    if (confirmButtonText.isEmpty()) {
        confirmButtonText = yesButton->text();
    }
    m_psbt_button = button(QMessageBox::Save);
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &SendConfirmationDialog::countDown);
}

int SendConfirmationDialog::exec()
{
    updateButtons();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateButtons()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + (m_enable_send ? (" (" + QString::number(secDelay) + ")") : QString("")));
        if (m_psbt_button) {
            m_psbt_button->setEnabled(false);
            m_psbt_button->setText(m_psbt_button_text + " (" + QString::number(secDelay) + ")");
        }
    }
    else
    {
        yesButton->setEnabled(m_enable_send);
        yesButton->setText(confirmButtonText);
        if (m_psbt_button) {
            m_psbt_button->setEnabled(true);
            m_psbt_button->setText(m_psbt_button_text);
        }
    }
}
