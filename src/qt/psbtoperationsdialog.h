// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_PSBTOPERATIONSDIALOG_H
#define CONNECTCOIN_QT_PSBTOPERATIONSDIALOG_H

#include <QDialog>
#include <QPointer>
#include <QString>

#include <node/psbt.h>
#include <psbt.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/walletmodel.h>

#include <memory>

namespace Ui {
class PSBTOperationsDialog;
}

/** Dialog showing transaction details. */
class PSBTOperationsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PSBTOperationsDialog(QWidget* parent, WalletModel* walletModel, ClientModel* clientModel);
    ~PSBTOperationsDialog();

    void openWithPSBT(PartiallySignedTransaction psbtx);

public Q_SLOTS:
    void signTransaction();
    void broadcastTransaction();
    void copyToClipboard();
    void saveTransaction();

private:
    Ui::PSBTOperationsDialog* m_ui;
    // Immutable shared input keeps submitting a large PSBT cheap on the GUI.
    // Mutable copies are created by the backend task, never by a Qt action.
    std::shared_ptr<const PartiallySignedTransaction> m_transaction_data;
    QPointer<WalletModel> m_wallet_model;
    QPointer<ClientModel> m_client_model;

    enum class StatusLevel {
        Info,
        Warn,
        Error
    };

    struct TransactionDisplayData {
        PartiallySignedTransaction transaction;
        node::PSBTAnalysis analysis{};
        std::vector<bool> own_outputs{};
        size_t unsigned_inputs{0};
        size_t could_sign{0};
        bool private_keys_disabled{false};
        QString description{};
    };
    void updateTransactionDisplay();
    static QString renderTransaction(const TransactionDisplayData& data, BitcoinUnit display_unit);
    void showStatus(const QString &msg, StatusLevel level);
    void showTransactionStatus(const TransactionDisplayData& data);
};

#endif // CONNECTCOIN_QT_PSBTOPERATIONSDIALOG_H
