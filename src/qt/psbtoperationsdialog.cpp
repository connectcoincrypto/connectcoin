// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/psbtoperationsdialog.h>

#include <common/messages.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/psbt.h>
#include <node/types.h>
#include <policy/policy.h>
#include <qt/bitcoinunits.h>
#include <qt/forms/ui_psbtoperationsdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <fstream>
#include <iostream>
#include <string>

using common::TransactionErrorString;
using node::AnalyzePSBT;
using node::DEFAULT_MAX_RAW_TX_FEE_RATE;
using node::PSBTAnalysis;
using node::TransactionError;

PSBTOperationsDialog::PSBTOperationsDialog(
    QWidget* parent, WalletModel* wallet_model, ClientModel* client_model) : QDialog(parent, GUIUtil::dialog_flags),
                                                                             m_ui(new Ui::PSBTOperationsDialog),
                                                                             m_wallet_model(wallet_model),
                                                                             m_client_model(client_model)
{
    m_ui->setupUi(this);

    connect(m_ui->signTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::signTransaction);
    connect(m_ui->broadcastTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::broadcastTransaction);
    connect(m_ui->copyToClipboardButton, &QPushButton::clicked, this, &PSBTOperationsDialog::copyToClipboard);
    connect(m_ui->saveButton, &QPushButton::clicked, this, &PSBTOperationsDialog::saveTransaction);

    connect(m_ui->closeButton, &QPushButton::clicked, this, &PSBTOperationsDialog::close);

    m_ui->signTransactionButton->setEnabled(false);
    m_ui->broadcastTransactionButton->setEnabled(false);
    if (m_wallet_model) connect(m_wallet_model, &QObject::destroyed, this, &QDialog::close);
    if (m_client_model) connect(m_client_model, &QObject::destroyed, this, &QDialog::close);
}

PSBTOperationsDialog::~PSBTOperationsDialog()
{
    delete m_ui;
}

void PSBTOperationsDialog::openWithPSBT(PartiallySignedTransaction psbtx)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_client_model) return;
    struct LoadResult {
        PartiallySignedTransaction transaction;
        bool complete{false};
        size_t could_sign{0};
        bool private_keys_disabled{false};
        std::optional<PSBTError> error{};
    };
    auto load = [transaction = std::move(psbtx)](interfaces::Wallet* wallet) mutable {
        LoadResult result{std::move(transaction)};
        result.complete = FinalizePSBT(result.transaction);
        if (wallet) {
            result.error = wallet->fillPSBT({.sign = false, .bip32_derivs = true}, &result.could_sign, result.transaction, result.complete);
            result.private_keys_disabled = wallet->privateKeysDisabled();
        }
        return result;
    };
    auto result = m_wallet_model
        ? GUIUtil::WaitForBackendTask(m_wallet_model->requestWalletData([load = std::move(load)](interfaces::Wallet& wallet) mutable { return load(&wallet); }), this)
        : GUIUtil::WaitForBackendTask(m_client_model->requestNodeData([load = std::move(load)](interfaces::Node&) mutable { return load(nullptr); }), this);
    if (!guard) return;
    m_transaction_data = std::make_shared<const PartiallySignedTransaction>(std::move(result.transaction));
    if (m_wallet_model) {
        if (result.error) {
            showStatus(tr("Failed to load transaction: %1")
                           .arg(QString::fromStdString(PSBTErrorString(*result.error).translated)),
                       StatusLevel::Error);
            return;
        }
        m_ui->signTransactionButton->setEnabled(!result.complete && !result.private_keys_disabled && result.could_sign > 0);
    } else {
        m_ui->signTransactionButton->setEnabled(false);
    }

    m_ui->broadcastTransactionButton->setEnabled(result.complete);

    updateTransactionDisplay();
}

void PSBTOperationsDialog::signTransaction()
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_wallet_model || !m_client_model || !m_transaction_data) return;
    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!guard || !m_wallet_model || !m_client_model) return;
    struct SignResult {
        PartiallySignedTransaction transaction;
        bool complete{false};
        size_t count{0};
        std::optional<PSBTError> error{};
    };
    auto result = GUIUtil::WaitForBackendTask(m_wallet_model->requestWalletData(
        [transaction = m_transaction_data](interfaces::Wallet& wallet) {
            SignResult result{*transaction};
            result.error = wallet.fillPSBT({.sign = true, .bip32_derivs = true}, &result.count, result.transaction, result.complete);
            return result;
        }), this);
    if (!guard) return;
    m_transaction_data = std::make_shared<const PartiallySignedTransaction>(std::move(result.transaction));
    const auto& err = result.error;
    const bool complete = result.complete;
    const size_t n_signed = result.count;

    if (err) {
        showStatus(tr("Failed to sign transaction: %1")
            .arg(QString::fromStdString(PSBTErrorString(*err).translated)), StatusLevel::Error);
        return;
    }

    updateTransactionDisplay();

    if (!guard) return;
    if (!complete && !ctx.isValid()) {
        showStatus(tr("Cannot sign inputs while wallet is locked."), StatusLevel::Warn);
    } else if (!complete && n_signed < 1) {
        showStatus(tr("Could not sign any more inputs."), StatusLevel::Warn);
    } else if (!complete) {
        showStatus(tr("Signed %n input(s), but more signatures are still required.", "", n_signed),
            StatusLevel::Info);
    } else {
        showStatus(tr("Signed transaction successfully. Transaction is ready to broadcast."),
            StatusLevel::Info);
        m_ui->broadcastTransactionButton->setEnabled(true);
    }
}

void PSBTOperationsDialog::broadcastTransaction()
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_client_model || !m_transaction_data) return;
    struct BroadcastResult {
        CTransactionRef transaction;
        TransactionError error{TransactionError::OK};
    };
    const auto result = GUIUtil::WaitForBackendTask(m_client_model->requestNodeData(
        [input = m_transaction_data](interfaces::Node& node) {
            BroadcastResult result;
            auto psbt = *input;
            CMutableTransaction transaction;
            if (!FinalizeAndExtractPSBT(psbt, transaction)) return result;
            result.transaction = MakeTransactionRef(std::move(transaction));
            std::string error;
            result.error = node.broadcastTransaction(result.transaction, DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK(), error);
            return result;
        }), this);
    if (!guard) return;
    if (!result.transaction) {
        // This is never expected to fail unless we were given a malformed PSBT
        // (e.g. with an invalid signature.)
        showStatus(tr("Unknown error processing transaction."), StatusLevel::Error);
        return;
    }

    if (result.error == TransactionError::OK) {
        showStatus(tr("Transaction broadcast successfully! Transaction ID: %1")
            .arg(QString::fromStdString(result.transaction->GetHash().GetHex())), StatusLevel::Info);
    } else {
        showStatus(tr("Transaction broadcast failed: %1")
            .arg(QString::fromStdString(TransactionErrorString(result.error).translated)), StatusLevel::Error);
    }
}

void PSBTOperationsDialog::copyToClipboard() {
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_client_model || !m_transaction_data) return;
    const auto encoded = GUIUtil::WaitForBackendTask(m_client_model->requestNodeData(
        [transaction = m_transaction_data](interfaces::Node&) {
            DataStream stream;
            stream << *transaction;
            return QString::fromStdString(EncodeBase64(stream.str()));
        }), this);
    if (!guard) return;
    GUIUtil::setClipboard(encoded);
    showStatus(tr("PSBT copied to clipboard."), StatusLevel::Info);
}

void PSBTOperationsDialog::saveTransaction() {
    GUIUtil::BackendOperationGuard operation;
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_client_model || !m_transaction_data) return;

    auto [filename_suggestion, serialized] = GUIUtil::WaitForBackendTask(m_client_model->requestNodeData(
        [transaction = m_transaction_data, unit = m_client_model->getOptionsModel()->getDisplayUnit()](interfaces::Node&) {
            QString suggestion;
            for (const auto& output : transaction->outputs) {
                if (!suggestion.isEmpty()) suggestion.append("-");
                CTxDestination address;
                ExtractDestination(output.script, address);
                suggestion.append(QString::fromStdString(EncodeDestination(address)) + "-" + BitcoinUnits::format(unit, output.amount));
            }
            suggestion.append(".psbt");
            DataStream stream;
            stream << *transaction;
            return std::make_pair(suggestion, stream.str());
        }), this);
    if (!guard) return;
    QString selected_filter;
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Save Transaction Data"), filename_suggestion,
        //: Expanded name of the binary PSBT file format. See: BIP 174.
        tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selected_filter);
    if (!guard || !m_client_model || filename.isEmpty()) {
        return;
    }
    const bool saved = GUIUtil::WaitForBackendTask(m_client_model->requestNodeData(
        [serialized = std::move(serialized), path = GUIUtil::QStringToPath(filename)](interfaces::Node&) {
            std::ofstream out{path.std_path(), std::ofstream::out | std::ofstream::binary};
            out << serialized;
            out.close();
            return !out.fail();
        }), this);
    if (!guard) return;
    if (!saved) {
        showStatus(tr("Failed to save PSBT to disk."), StatusLevel::Error);
        return;
    }
    showStatus(tr("PSBT saved to disk."), StatusLevel::Info);
}

void PSBTOperationsDialog::updateTransactionDisplay() {
    const QPointer<PSBTOperationsDialog> guard{this};
    if (!m_client_model || !m_transaction_data) return;
    auto analyze = [transaction = m_transaction_data, unit = m_client_model->getOptionsModel()->getDisplayUnit()](interfaces::Wallet* wallet) {
        TransactionDisplayData result{*transaction};
        if (wallet) {
            bool complete{false};
            const auto err = wallet->fillPSBT({.sign = false, .bip32_derivs = false}, &result.could_sign, result.transaction, complete);
            if (err) result.could_sign = 0;
            result.private_keys_disabled = wallet->privateKeysDisabled();
        }
        for (const auto& output : result.transaction.outputs) {
            result.own_outputs.push_back(wallet && wallet->txoutIsMine(CTxOut(output.amount, output.script)));
        }
        result.analysis = AnalyzePSBT(result.transaction);
        result.unsigned_inputs = CountPSBTUnsignedInputs(result.transaction);
        result.description = renderTransaction(result, unit);
        return result;
    };
    auto result = m_wallet_model
        ? GUIUtil::WaitForBackendTask(m_wallet_model->requestWalletData([analyze = std::move(analyze)](interfaces::Wallet& wallet) mutable { return analyze(&wallet); }), this)
        : GUIUtil::WaitForBackendTask(m_client_model->requestNodeData([analyze = std::move(analyze)](interfaces::Node&) mutable { return analyze(nullptr); }), this);
    if (!guard) return;
    m_ui->transactionDescription->setText(result.description);
    showTransactionStatus(result);
    m_transaction_data = std::make_shared<const PartiallySignedTransaction>(std::move(result.transaction));
}

QString PSBTOperationsDialog::renderTransaction(const TransactionDisplayData& data, BitcoinUnit display_unit)
{
    QString tx_description;
    QLatin1String bullet_point(" * ");
    CAmount totalAmount = 0;
    size_t output_index{0};
    for (const PSBTOutput& out : data.transaction.outputs) {
        CTxDestination address;
        ExtractDestination(out.script, address);
        totalAmount += out.amount;
        tx_description.append(bullet_point).append(tr("Sends %1 to %2")
            .arg(BitcoinUnits::formatWithUnit(BitcoinUnit::BTC, out.amount))
            .arg(QString::fromStdString(EncodeDestination(address))));
        // Check if the address is one of ours
        if (data.own_outputs[output_index++]) tx_description.append(" (" + tr("own address") + ")");
        tx_description.append("<br>");
    }

    const PSBTAnalysis& analysis = data.analysis;
    tx_description.append(bullet_point);
    if (!analysis.fee) {
        // This happens if the transaction is missing input UTXO information.
        tx_description.append(tr("Unable to calculate transaction fee or total transaction amount."));
    } else {
        tx_description.append(tr("Pays transaction fee: "));
        tx_description.append(BitcoinUnits::formatWithUnit(BitcoinUnit::BTC, *analysis.fee));

        // add total amount in all subdivision units
        tx_description.append("<hr />");
        QStringList alternativeUnits;
        for (const BitcoinUnits::Unit u : BitcoinUnits::availableUnits())
        {
            if(u != display_unit) {
                alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
            }
        }
        tx_description.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
            .arg(BitcoinUnits::formatHtmlWithUnit(display_unit, totalAmount)));
        tx_description.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
            .arg(alternativeUnits.join(" " + tr("or") + " ")));
    }

    size_t num_unsigned = data.unsigned_inputs;
    if (num_unsigned > 0) {
        tx_description.append("<br><br>");
        tx_description.append(tr("Transaction has %n unsigned input(s).", "", num_unsigned));
    }

    return tx_description;
}

void PSBTOperationsDialog::showStatus(const QString &msg, StatusLevel level) {
    m_ui->statusBar->setText(msg);
    switch (level) {
        case StatusLevel::Info: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : lightgreen }");
            break;
        }
        case StatusLevel::Warn: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : orange }");
            break;
        }
        case StatusLevel::Error: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : red }");
            break;
        }
    }
    m_ui->statusBar->show();
}

void PSBTOperationsDialog::showTransactionStatus(const TransactionDisplayData& data) {
    const PSBTAnalysis& analysis = data.analysis;
    size_t n_could_sign = data.could_sign;

    switch (analysis.next) {
        case PSBTRole::UPDATER: {
            showStatus(tr("Transaction is missing some information about inputs."), StatusLevel::Warn);
            break;
        }
        case PSBTRole::SIGNER: {
            QString need_sig_text = tr("Transaction still needs signature(s).");
            StatusLevel level = StatusLevel::Info;
            if (!m_wallet_model) {
                need_sig_text += " " + tr("(But no wallet is loaded.)");
                level = StatusLevel::Warn;
            } else if (data.private_keys_disabled) {
                need_sig_text += " " + tr("(But this wallet cannot sign transactions.)");
                level = StatusLevel::Warn;
            } else if (n_could_sign < 1) {
                need_sig_text += " " + tr("(But this wallet does not have the right keys.)"); // XXX wording
                level = StatusLevel::Warn;
            }
            showStatus(need_sig_text, level);
            break;
        }
        case PSBTRole::FINALIZER:
        case PSBTRole::EXTRACTOR: {
            showStatus(tr("Transaction is fully signed and ready for broadcast."), StatusLevel::Info);
            break;
        }
        default: {
            showStatus(tr("Transaction status is unknown."), StatusLevel::Error);
            break;
        }
    }
}
