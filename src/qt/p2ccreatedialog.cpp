// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/p2ccreatedialog.h>

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <consensus/p2c.h>
#include <interfaces/wallet.h>
#include <node/interface_ui.h>
#include <policy/feerate.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <uint256.h>
#include <util/result.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/p2c.h>
#include <wallet/p2c_tls.h>
#include <wallet/wallet.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

struct P2CCreateDialog::RsaProbeState
{
    std::atomic<bool> cancelled{false};
    std::atomic<bool> rsa_verified{false};
    const std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() + std::chrono::seconds{3}};
};

P2CCreateDialog::P2CCreateDialog(QWidget* parent) : QWidget(parent), m_rsa_probe{wallet::ProbeP2CRsa}
{
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(tr("Create pay-to-connect bounties"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);
    auto* explanation = new QLabel(tr("Fund independent rewards for valid TLS connection proofs. During the three-second confirmation, this page checks the domain's RSA signature support with a TLS handshake. No HTTP request is sent."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    m_form = new QWidget(this);
    auto* form = new QFormLayout(m_form);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_domain = new QLineEdit(m_form);
    m_domain->setObjectName("p2cDomain");
    m_domain->setMaxLength(253);
    m_domain->setPlaceholderText("example.com");
    m_domain->setToolTip(tr("Lower-case ASCII domain (use punycode for international domains), without a URL scheme, path, port or trailing dot."));
    form->addRow(tr("&Domain:"), m_domain);
    m_amount = new BitcoinAmountField(m_form);
    m_amount->setObjectName("p2cAmount");
    m_amount->SetMinValue(1);
    m_amount->SetMaxValue(MAX_MONEY);
    form->addRow(tr("Reward per &output:"), m_amount);
    m_count = new QSpinBox(m_form);
    m_count->setObjectName("p2cOutputCount");
    m_count->setRange(1, static_cast<int>(wallet::MAX_P2C_OUTPUT_COUNT));
    form->addRow(tr("&Number of outputs:"), m_count);
    m_work_mode = new QComboBox(m_form);
    m_work_mode->setObjectName("p2cWorkMode");
    m_work_mode->addItems({tr("Leading zero bits"), tr("Maximum hash")});
    form->addRow(tr("Difficulty &format:"), m_work_mode);
    m_bits = new QSpinBox(m_form);
    m_bits->setObjectName("p2cWorkBits");
    m_bits->setRange(0, 256);
    m_bits->setValue(10);
    form->addRow(tr("&Zero bits:"), m_bits);
    m_target = new QLineEdit(m_form);
    m_target->setObjectName("p2cTarget");
    m_target->setMaxLength(64);
    m_target->setPlaceholderText(tr("Exactly 64 hexadecimal characters"));
    form->addRow(tr("Maximum &hash:"), m_target);
    m_roots = new QComboBox(m_form);
    m_roots->setObjectName("p2cRootsVersion");
    m_roots->addItem(tr("Version %1").arg(P2C_ROOT_CERTIFICATES_VERSION_1), P2C_ROOT_CERTIFICATES_VERSION_1);
    form->addRow(tr("Trusted root &certificates:"), m_roots);
    m_custom_fee = new QCheckBox(tr("Use a custom fee rate"), m_form);
    m_custom_fee->setObjectName("p2cCustomFee");
    form->addRow(m_custom_fee);
    m_fee_rate = new BitcoinAmountField(m_form);
    m_fee_rate->setObjectName("p2cFeeRate");
    m_fee_rate->SetMinValue(1);
    m_fee_rate->SetMaxValue(MAX_MONEY);
    m_fee_rate->setEnabled(false);
    form->addRow(tr("Fee per 1,000 virtual bytes:"), m_fee_rate);
    m_create = new QPushButton(tr("&Review P2C…"), m_form);
    m_create->setObjectName("p2cCreateButton");
    form->addRow(m_create);
    layout->addWidget(m_form);
    m_status = new QLabel(this);
    m_status->setObjectName("p2cStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_receipt = new QPlainTextEdit(this);
    m_receipt->setObjectName("p2cReceipt");
    m_receipt->setReadOnly(true);
    m_receipt->hide();
    layout->addWidget(m_receipt);
    layout->addStretch();
    m_form->setEnabled(false);
    connect(m_work_mode, &QComboBox::currentIndexChanged, this, &P2CCreateDialog::updateWorkMode);
    connect(m_bits, &QSpinBox::valueChanged, this, &P2CCreateDialog::updateWorkMode);
    connect(m_custom_fee, &QCheckBox::toggled, m_fee_rate, &BitcoinAmountField::setEnabled);
    GUIUtil::ExceptionSafeConnect(m_create, &QPushButton::clicked, this, &P2CCreateDialog::prepare);
    updateWorkMode();
}

P2CCreateDialog::~P2CCreateDialog()
{
    cancelRsaProbe();
}

void P2CCreateDialog::setRsaProbeForTest(RsaProbe probe)
{
    m_rsa_probe = std::move(probe);
}

void P2CCreateDialog::cancelRsaProbe()
{
    if (m_probe) m_probe->cancelled.store(true);
    m_probe.reset();
}

void P2CCreateDialog::startRsaProbe(const std::string& domain, uint32_t roots_version)
{
    const auto state = std::make_shared<RsaProbeState>();
    m_probe = state;
    auto* timer = new QTimer(m_confirmation);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, m_confirmation, [this, state] { freezeSignatureAlgorithms(state); });
    timer->start(3000);
    // DNS can outlive cancellation. Bound detached workers across all pages;
    // a busy probe service conservatively leaves the full algorithm mask.
    static const auto active_probes = std::make_shared<std::atomic<int>>(0);
    const auto active = active_probes;
    if (active->fetch_add(1) >= 4) {
        --*active;
        return;
    }
    try {
        // No QObject, wallet, or batch is captured. A resolver that returns
        // after cancellation can only touch this isolated shared state.
        std::thread([state, domain, roots_version, probe = m_rsa_probe, active] {
            try {
                const auto cancelled = [state] { return state->cancelled.load(); };
                const bool verified = probe && probe(domain, roots_version, cancelled, state->deadline);
                if (verified && !cancelled() && std::chrono::steady_clock::now() < state->deadline) {
                    state->rsa_verified.store(true);
                }
            } catch (...) {
                // Probe failure is advisory: retain all supported algorithms.
            }
            --*active;
        }).detach();
    } catch (const std::exception&) {
        --*active;
        // Thread creation failure has the same conservative result as timeout.
    }
}

void P2CCreateDialog::freezeSignatureAlgorithms(const std::shared_ptr<RsaProbeState>& probe)
{
    if (m_probe != probe || !m_confirmation || !m_batch || !m_model) return;
    const uint8_t mask = probe->rsa_verified.load() ? PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA
                                                   : PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL;
    cancelRsaProbe();
    auto selected = m_batch->SelectSignatureAlgorithmsMask(mask);
    if (!selected) {
        const auto error = QString::fromStdString(util::ErrorString(selected).translated);
        m_confirmation->reject();
        showError(error);
        return;
    }
    const QString algorithms = mask == PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA
        ? tr("RSA-PSS with SHA-256") : tr("All supported algorithms");
    const QString selection = tr("Allowed signatures: %1 (mask %2)").arg(algorithms).arg(static_cast<unsigned int>(mask));
    m_confirmation->setInformativeText(tr("These rewards can be claimed by anyone presenting a valid connection proof. You cannot recover them using a normal wallet signature. Review all transactions before sending.") + "<br /><br />" + selection);
    m_confirmation->setDetailedText(m_confirmation->detailedText() + '\n' + selection);
    // The batch and visible selection are now frozen; late probe results never
    // reach either. Selection needs no unlock, fee calculation, or re-signing.
    m_ready_to_send = true;
    m_confirmation->button(QMessageBox::Yes)->setEnabled(true);
}

void P2CCreateDialog::setModel(WalletModel* model)
{
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    cancelRsaProbe();
    if (m_confirmation) m_confirmation->reject();
    m_batch.reset();
    m_model = model;
    m_form->setEnabled(model != nullptr);
    if (!model) return;
    const auto unit = model->getOptionsModel()->getDisplayUnit();
    m_amount->setDisplayUnit(unit);
    m_fee_rate->setDisplayUnit(unit);
    connect(model, &QObject::destroyed, this, [this] { setModel(nullptr); });
    const bool supported{!model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner()};
    m_create->setEnabled(supported);
    m_status->setText(supported ? tr("Fees are calculated before approval. Large requests may be split into multiple transactions.")
                                : tr("P2C creation currently requires a wallet with local private keys. Watch-only and external-signer wallets are not supported on this page."));
}

void P2CCreateDialog::updateWorkMode()
{
    const bool bits_mode{m_work_mode->currentIndex() == 0};
    m_bits->setEnabled(bits_mode);
    m_target->setEnabled(!bits_mode);
    if (bits_mode) {
        arith_uint256 target{~arith_uint256{0}};
        target >>= m_bits->value();
        m_target->setText(QString::fromStdString(ArithToUint256(target).GetHex()));
    }
}

void P2CCreateDialog::showError(const QString& text)
{
    m_status->setText(text);
    Q_EMIT message(tr("Create P2C"), text, CClientUIInterface::MSG_ERROR);
}

void P2CCreateDialog::prepare(bool)
{
    if (!m_model || m_batch) return;
    const std::string domain{m_domain->text().toStdString()};
    if (!IsCanonicalP2CDomain(domain)) {
        showError(tr("Enter a canonical lower-case ASCII domain without a trailing dot, scheme, path or port."));
        return;
    }
    bool valid_amount{false};
    const CAmount amount{m_amount->value(&valid_amount)};
    const int count{m_count->value()};
    if (!valid_amount || amount <= 0 || !MoneyRange(amount) || count > MAX_MONEY / amount) {
        showError(tr("Enter a positive reward whose total does not exceed the money limit."));
        return;
    }
    const auto target{uint256::FromHex(m_target->text().toStdString())};
    if (!target) {
        showError(tr("The maximum hash must contain exactly 64 hexadecimal characters."));
        return;
    }
    wallet::CCoinControl control;
    if (m_custom_fee->isChecked()) {
        bool valid_fee{false};
        const CAmount fee_rate{m_fee_rate->value(&valid_fee)};
        if (!valid_fee || fee_rate <= 0 || !MoneyRange(fee_rate)) {
            showError(tr("Enter a positive fee rate per 1,000 virtual bytes."));
            return;
        }
        control.m_feerate = CFeeRate(fee_rate);
    }
    const wallet::CRecipient recipient{
        .dest = CNoDestination{}, .nAmount = amount, .fSubtractFeeFromAmount = false,
        .p2c = PayToDomainOutput{domain, *target, m_roots->currentData().toUInt(), PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL},
    };
    const bool bits_mode{m_work_mode->currentIndex() == 0};
    const int work_bits{m_bits->value()};
    QString preparation_error;
    const QPointer<P2CCreateDialog> guard(this);
    const QPointer<WalletModel> model(m_model);
    {
        auto unlock{model->requestUnlock()};
        // Unlocking can enter a nested event loop; do not continue with a
        // deleted page or a different wallet after that callback returns.
        if (!guard || !model || m_model != model || !unlock.isValid()) return;
        try {
            auto prepared{model->wallet().prepareP2CTransactions(recipient, count, control)};
            if (!prepared) {
                preparation_error = QString::fromStdString(util::ErrorString(prepared).translated);
            } else {
                auto alternative = (*prepared)->PrepareSignatureAlgorithmsAlternative(PayToDomainOutput::SIGNATURE_ALGORITHMS_RSA);
                if (!alternative) {
                    preparation_error = QString::fromStdString(util::ErrorString(alternative).translated);
                } else {
                    m_batch = std::move(*prepared);
                }
            }
        } catch (const std::exception& e) {
            preparation_error = tr("Unable to prepare P2C transactions: %1").arg(QString::fromUtf8(e.what()));
        }
    }
    if (!m_batch) {
        // Relock the wallet before an error popup can enter a nested event loop.
        showError(preparation_error);
        return;
    }
    const CAmount reward_total{amount * count};
    if (m_batch->GetFee() > MAX_MONEY - reward_total) {
        m_batch.reset();
        showError(tr("The reward total plus fees exceeds the money limit."));
        return;
    }
    const auto unit = m_model->getOptionsModel()->getDisplayUnit();
    const auto format = [unit](CAmount value) { return BitcoinUnits::formatHtmlWithUnit(unit, value); };
    const QString summary = tr("Wallet: %1<br />Domain: %2<br />Outputs: %3<br />Reward per output: %4<br />Total rewards: %5<br />Transactions: %6<br />Total fees: %7<br /><b>Total debit: %8</b>")
        .arg(GUIUtil::HtmlEscape(m_model->getWalletName()), GUIUtil::HtmlEscape(QString::fromStdString(domain)))
        .arg(count).arg(format(amount), format(reward_total)).arg(m_batch->GetTransactions().size())
        .arg(format(m_batch->GetFee()), format(reward_total + m_batch->GetFee()));
    const QString target_hex{QString::fromStdString(target->GetHex())};
    const auto roots_version{recipient.p2c->root_certificates_version};
    const QString details = tr("Maximum work hash: %1\nRoot certificates version: %2").arg(target_hex).arg(roots_version);
    const QString work_summary = bits_mode
        ? tr("Required leading zero bits: %1").arg(work_bits)
        : tr("Maximum work hash: %1").arg(target_hex);
    m_confirmation = new QMessageBox(QMessageBox::Question, tr("Confirm P2C creation"), summary + "<br />" + work_summary + "<br />" + tr("Root certificates version: %1").arg(roots_version),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    m_confirmation->setObjectName("p2cConfirmation");
    m_confirmation->setTextFormat(Qt::RichText);
    m_confirmation->setInformativeText(tr("These rewards can be claimed by anyone presenting a valid connection proof. You cannot recover them using a normal wallet signature. Review all transactions before sending.") + "<br /><br />" + tr("Checking RSA signature support…"));
    m_confirmation->setDetailedText(details);
    m_confirmation->setDefaultButton(QMessageBox::Cancel);
    m_confirmation->setAttribute(Qt::WA_DeleteOnClose);
    auto* send = m_confirmation->button(QMessageBox::Yes);
    send->setText(tr("Send P2C"));
    send->setEnabled(false);
    m_ready_to_send = false;
    GUIUtil::ExceptionSafeConnect(m_confirmation.data(), &QDialog::finished, this, &P2CCreateDialog::finishConfirmation);
    m_form->setEnabled(false);
    // No nested event loop: closing/unloading the wallet safely destroys the
    // dialog and releases the prepared batch without sending anything.
    m_confirmation->open();
    startRsaProbe(domain, roots_version);
}

void P2CCreateDialog::finishConfirmation(int result)
{
    cancelRsaProbe();
    if (!m_batch) return;
    const bool approved = result == QMessageBox::Yes && m_ready_to_send;
    m_ready_to_send = false;
    auto batch = std::move(m_batch);
    m_confirmation = nullptr;
    QString error;
    std::vector<Txid> submitted;
    if (approved && m_model) {
        auto committed{batch->Commit()};
        QStringList txids;
        size_t index{0};
        for (const auto& [created, count] : batch->GetTransactions()) {
            if (index++ >= batch->GetCommittedCount()) break;
            txids.append(QString::fromStdString(created.tx->GetHash().GetHex()));
            submitted.push_back(created.tx->GetHash());
        }
        m_receipt->setPlainText(txids.join('\n'));
        m_receipt->setVisible(!txids.isEmpty());
        if (committed) {
            m_status->setText(tr("P2C transactions submitted to the wallet (%1). Transaction IDs:").arg(txids.size()));
            m_amount->clear();
        } else {
            error = QString::fromStdString(util::ErrorString(committed).translated);
        }
    } else {
        m_status->setText(tr("Cancelled. No P2C transactions were sent."));
    }
    batch.reset();
    m_form->setEnabled(m_model != nullptr);
    // Release reservations and finalize state before callbacks can enter a
    // nested event loop (for example an error popup) or unload this wallet.
    QPointer<P2CCreateDialog> guard(this);
    for (const auto& txid : submitted) {
        Q_EMIT coinsSent(txid);
        if (!guard) return;
    }
    if (!error.isEmpty()) showError(error);
}
