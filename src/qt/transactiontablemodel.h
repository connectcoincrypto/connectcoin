// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_TRANSACTIONTABLEMODEL_H
#define CONNECTCOIN_QT_TRANSACTIONTABLEMODEL_H

#include <qt/bitcoinunits.h>

#include <primitives/transaction_identifier.h>

#include <QAbstractTableModel>
#include <QStringList>

#include <future>
#include <memory>
#include <set>
#include <string>

namespace interfaces {
class Handler;
}

class PlatformStyle;
class TransactionRecord;
class TransactionTablePriv;
class TransactionNotificationQueue;
class WalletModel;

/** UI model for the transaction table of a wallet.
 */
class TransactionTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit TransactionTableModel(const PlatformStyle *platformStyle, WalletModel *parent = nullptr);
    ~TransactionTableModel();

    //! Quiesce callbacks before the wallet drains its worker with a live GUI loop.
    void interrupt();

    enum ColumnIndex {
        Status = 0,
        Date = 1,
        Type = 2,
        ToAddress = 3,
        Amount = 4
    };

    /** Roles to get specific information from a transaction row.
        These are independent of column.
    */
    enum RoleIndex {
        /** Type of transaction */
        TypeRole = Qt::UserRole,
        /** Date and time this transaction was created */
        DateRole,
        /** Address of transaction */
        AddressRole,
        /** Label of address related to transaction */
        LabelRole,
        /** Net amount of transaction */
        AmountRole,
        /** Transaction hash */
        TxHashRole,
        /** Whole transaction as plain text */
        TxPlainTextRole,
        /** Is transaction confirmed? */
        ConfirmedRole,
        /** Formatted amount, without brackets when unconfirmed */
        FormattedAmountRole,
        /** Transaction status (TransactionRecord::Status) */
        StatusRole,
        /** Unprocessed icon */
        RawDecorationRole,
    };

    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex & parent = QModelIndex()) const override;
    //! Find the transaction's rows in the hash-sorted cache without scanning
    //! unrelated history or requesting transaction status updates.
    QModelIndexList indexesForTransaction(const Txid& txid) const;
    bool processingQueuedTransactions() const { return fProcessingQueuedTransactions; }

    //! Explicit actions snapshot the selected record before dispatch. Model
    //! data() itself must never wait or reenter the GUI event loop.
    std::future<QString> requestTxDescription(const QModelIndex& source_index) const;
    std::future<QString> requestTxHex(const QModelIndex& source_index) const;

Q_SIGNALS:
    //! Repaint visible confirmations after a tip or cosmetic status snapshot
    //! change, without refiltering/resorting unchanged history identities.
    void confirmationsChanged();

private:
    WalletModel *walletModel;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::shared_ptr<TransactionNotificationQueue> m_notifications;
    QStringList columns;
    TransactionTablePriv *priv;
    bool fProcessingQueuedTransactions{false};
    bool m_stopped{false};
    bool m_label_update_pending{false};
    bool m_all_labels_dirty{false};
    int m_next_label_row{0};
    std::set<std::string> m_changed_label_addresses;
    const PlatformStyle *platformStyle;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();
    void pollTransactionUpdates();
    void pollStatusUpdates();
    void processLabelUpdates();

    QString lookupAddress(const std::string &address, bool tooltip) const;
    QVariant addressColor(const TransactionRecord *wtx) const;
    QString formatTxStatus(const TransactionRecord *wtx) const;
    QString formatTxDate(const TransactionRecord *wtx) const;
    QString formatTxType(const TransactionRecord *wtx) const;
    QString formatTxToAddress(const TransactionRecord *wtx, bool tooltip) const;
    QString formatTxAmount(const TransactionRecord *wtx, bool showUnconfirmed=true, BitcoinUnits::SeparatorStyle separators=BitcoinUnits::SeparatorStyle::STANDARD) const;
    QString formatTooltip(const TransactionRecord *rec) const;
    QVariant txStatusDecoration(const TransactionRecord *wtx) const;
    QVariant txAddressDecoration(const TransactionRecord *wtx) const;

public Q_SLOTS:
    /* New transaction, or transaction changed status */
    void updateTransaction(const QString &hash, int status, bool showTransaction);
    void updateConfirmations();
    void updateAddressBookLabels(const QString& address);
    void updateDisplayUnit();
    /** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
    void updateAmountColumnTitle();
    /* Needed to update fProcessingQueuedTransactions through a QueuedConnection */
    void setProcessingQueuedTransactions(bool value) { if (!m_stopped) fProcessingQueuedTransactions = value; }

    friend class TransactionTablePriv;

private Q_SLOTS:
    void processBackendNotifications();
};

#endif // CONNECTCOIN_QT_TRANSACTIONTABLEMODEL_H
