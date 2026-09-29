// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactiontablemodel.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiondesc.h>
#include <qt/transactionrecord.h>
#include <qt/walletmodel.h>

#include <core_io.h>
#include <interfaces/handler.h>
#include <tinyformat.h>
#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QIcon>
#include <QLatin1Char>
#include <QLatin1String>
#include <QList>
#include <QScopedValueRollback>
#include <QTimer>


// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
        Qt::AlignLeft|Qt::AlignVCenter, /*status=*/
        Qt::AlignLeft|Qt::AlignVCenter, /*date=*/
        Qt::AlignLeft|Qt::AlignVCenter, /*type=*/
        Qt::AlignLeft|Qt::AlignVCenter, /*address=*/
        Qt::AlignRight|Qt::AlignVCenter /* amount */
    };

// Comparison operator for sort/binary search of model tx list
struct TxLessThan
{
    bool operator()(const TransactionRecord &a, const TransactionRecord &b) const
    {
        return a.hash < b.hash;
    }
    bool operator()(const TransactionRecord &a, const Txid &b) const
    {
        return a.hash < b;
    }
    bool operator()(const Txid &a, const TransactionRecord &b) const
    {
        return a < b.hash;
    }
};

// Core can notify thousands of transactions while holding wallet locks. Keep
// their ordering (including rescan progress) without posting a Qt event for
// every notification. The shared gate also covers a callback already running
// when the table disconnects and is destroyed.
class TransactionNotificationQueue
{
public:
    using Event = std::variant<std::pair<Txid, ChangeType>, int>;
    explicit TransactionNotificationQueue(TransactionTableModel* model) : target(model) {}
    std::mutex mutex;
    TransactionTableModel* target;
    std::deque<Event> events;
    bool scheduled{false};
    std::optional<bool> loading;

    void push(Event event)
    {
        std::lock_guard lock{mutex};
        if (!target) return;
        if (const auto* progress = std::get_if<int>(&event)) {
            // The table uses progress only to enter/leave rescan buffering.
            // Preserve the first state (even if subscribing mid-rescan) and
            // transitions in transaction order, not every percentage event.
            const bool next_loading = *progress < 100;
            if (loading == next_loading) return;
            loading = next_loading;
        }
        events.push_back(std::move(event));
        if (scheduled) return;
        scheduled = true;
        const bool invoked = QMetaObject::invokeMethod(target, "processBackendNotifications", Qt::QueuedConnection);
        assert(invoked);
    }
};

// queue notifications to show a non freezing progress dialog e.g. for rescan
struct TransactionNotification
{
public:
    TransactionNotification() = default;
    TransactionNotification(Txid _hash, ChangeType _status, bool _showTransaction):
        hash(_hash), status(_status), showTransaction(_showTransaction) {}

    void invoke(QObject *ttm)
    {
        QString strHash = QString::fromStdString(hash.GetHex());
        qDebug() << "NotifyTransactionChanged: " + strHash + " status= " + QString::number(status);
        bool invoked = QMetaObject::invokeMethod(ttm, "updateTransaction", Qt::DirectConnection,
                                  Q_ARG(QString, strHash),
                                  Q_ARG(int, status),
                                  Q_ARG(bool, showTransaction));
        assert(invoked);
    }
private:
    Txid hash;
    ChangeType status;
    bool showTransaction;
};

// Private implementation
class TransactionTablePriv
{
public:
    explicit TransactionTablePriv(TransactionTableModel *_parent) :
        parent(_parent)
    {
    }

    TransactionTableModel *parent;

    //! Local cache of wallet sorted by transaction hash
    QList<TransactionRecord> cachedWallet;

    /** True when model finishes loading all wallet transactions on start */
    bool m_loaded = false;
    /** True when transactions are being notified, for instance when scanning */
    bool m_loading = false;
    std::deque<TransactionNotification> vQueueNotifications;
    bool dispatch_pending{false};
    std::future<QList<TransactionRecord>> initial_query;

    struct PendingTransaction {
        uint64_t revision;
        int status;
        bool processing_queued;
    };
    using TransactionRequest = std::pair<Txid, PendingTransaction>;
    struct TransactionSnapshot {
        bool found{false};
        QList<TransactionRecord> records;
    };
    uint64_t next_revision{0};
    std::map<Txid, PendingTransaction> pending_transactions;
    std::deque<Txid> transaction_queue;
    std::set<Txid> queued_transactions;
    std::vector<TransactionRequest> inflight_transactions;
    std::future<std::vector<TransactionSnapshot>> transaction_query;
    QTimer* refresh_timer{new QTimer(parent)};

    struct StatusRequest {
        uint64_t revision;
        bool force{false};
    };
    struct StatusSnapshot {
        std::optional<interfaces::WalletTxStatus> status;
        int num_blocks{0};
        int64_t block_time{0};
    };
    using StatusEntry = std::pair<Txid, StatusRequest>;
    std::map<Txid, StatusRequest> pending_status;
    std::deque<Txid> status_queue;
    std::set<Txid> queued_status;
    std::vector<StatusEntry> inflight_status;
    std::future<std::vector<StatusSnapshot>> status_query;
    std::vector<StatusSnapshot> completed_status;
    size_t next_status{0};
    qsizetype next_status_row{0};
    bool status_apply_queued{false};
    bool status_applying{false};
    QTimer* status_timer{new QTimer(parent)};

    void queueStatus(const Txid& hash, const uint256& block_hash, bool force = false)
    {
        if (parent->m_stopped || block_hash.IsNull()) return;
        const auto pending{pending_status.find(hash)};
        // A changing tip must not continuously supersede the same query on a
        // fast chain. Publish its real wallet tip, then refresh if still behind.
        if (pending != pending_status.end()) return;
        pending_status.insert_or_assign(hash, StatusRequest{++next_revision, force});
        if (queued_status.insert(hash).second) status_queue.push_back(hash);
        if (!status_timer->isActive()) status_timer->start();
    }

    void queueTransaction(const Txid& hash)
    {
        if (queued_transactions.insert(hash).second) transaction_queue.push_back(hash);
    }

    void NotifyTransactionChanged(const Txid& hash, ChangeType status);
    void DispatchNotifications();

    /* Update our model of the wallet incrementally, to synchronize our model of the wallet
       with that of the core.

       Call with transaction that was added, removed or changed.
     */
    // Return true when a new row needs an asynchronous wallet snapshot.
    bool updateWallet(const Txid& hash, int status, bool showTransaction, TransactionSnapshot* snapshot = nullptr)
    {
        qDebug() << "TransactionTablePriv::updateWallet: " + QString::fromStdString(hash.ToString()) + " " + QString::number(status);

        // Find bounds of this transaction in model
        QList<TransactionRecord>::iterator lower = std::lower_bound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        QList<TransactionRecord>::iterator upper = std::upper_bound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        int lowerIndex = (lower - cachedWallet.begin());
        int upperIndex = (upper - cachedWallet.begin());
        bool inModel = (lower != upper);

        if(status == CT_UPDATED)
        {
            if(showTransaction && !inModel)
                status = CT_NEW; /* Not in model, but want to show, treat as new */
            if(!showTransaction && inModel)
                status = CT_DELETED; /* In model, but want to hide, treat as deleted */
        }

        qDebug() << "    inModel=" + QString::number(inModel) +
                    " Index=" + QString::number(lowerIndex) + "-" + QString::number(upperIndex) +
                    " showTransaction=" + QString::number(showTransaction) + " derivedStatus=" + QString::number(status);

        switch(status)
        {
        case CT_NEW:
            if(inModel)
            {
                qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_NEW, but transaction is already in model";
                break;
            }
            if(showTransaction)
            {
                if (!snapshot) return true;
                if (!snapshot->found)
                {
                    qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_NEW, but transaction is not in wallet";
                    break;
                }
                // Added -- insert at the right position
                auto& toInsert = snapshot->records;
                if(!toInsert.isEmpty()) /* only if something to insert */
                {
                    parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex+toInsert.size()-1);
                    // Shift the existing tail once, not once per output of a
                    // large transaction. Address encoding/decomposition was
                    // already completed by the worker.
                    cachedWallet.insert(lowerIndex, toInsert.size(), TransactionRecord{});
                    std::move(toInsert.begin(), toInsert.end(), cachedWallet.begin() + lowerIndex);
                    parent->endInsertRows();
                }
            }
            break;
        case CT_DELETED:
            if(!inModel)
            {
                qWarning() << "TransactionTablePriv::updateWallet: Warning: Got CT_DELETED, but transaction is not in model";
                break;
            }
            // Removed -- remove entire transaction from table
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex-1);
            cachedWallet.erase(lower, upper);
            parent->endRemoveRows();
            break;
        case CT_UPDATED:
            // A conflict can hide a row, and a disconnected conflict must make
            // it visible again even when no view currently paints that row.
            // A transaction can have thousands of output rows. Record the
            // invalidation once; its bounded result application refreshes all
            // outputs even if the wallet tip has not changed.
            if (inModel) queueStatus(hash, parent->walletModel->getLastBlockProcessed(), /*force=*/true);
            break;
        }
        return false;
    }

    int size()
    {
        return cachedWallet.size();
    }

    TransactionRecord* index(const uint256& cur_block_hash, const int idx)
    {
        if (idx >= 0 && idx < cachedWallet.size()) {
            TransactionRecord *rec = &cachedWallet[idx];

            // Even a successful TRY_LOCK can lead to expensive ancestry and
            // descriptor work. Painting only reads the cache and queues work.
            if (!cur_block_hash.IsNull() && rec->statusUpdateNeeded(cur_block_hash)) {
                queueStatus(rec->hash, cur_block_hash);
            }
            return rec;
        }
        return nullptr;
    }

};

TransactionTableModel::TransactionTableModel(const PlatformStyle *_platformStyle, WalletModel *parent):
        QAbstractTableModel(parent),
        walletModel(parent),
        m_notifications(std::make_shared<TransactionNotificationQueue>(this)),
        priv(new TransactionTablePriv(this)),
        platformStyle(_platformStyle)
{
    subscribeToCoreSignals();
    connect(walletModel, &WalletModel::addressBookLabelsChanged,
            this, &TransactionTableModel::updateAddressBookLabels);
    connect(this, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int first, int last) {
        // Newly inserted rows already expose the latest cached labels. Keep
        // the pending scan positioned at the same surviving transaction.
        if (m_label_update_pending && first <= m_next_label_row) m_next_label_row += last - first + 1;
    });
    connect(this, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex&, int first, int last) {
        if (m_label_update_pending && first < m_next_label_row) {
            m_next_label_row -= std::min(m_next_label_row - first, last - first + 1);
        }
    });
    connect(this, &QAbstractItemModel::modelReset, this, [this] { m_next_label_row = 0; });

    columns << QString() << tr("Date") << tr("Type") << tr("Label") << BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());
    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &TransactionTableModel::updateDisplayUnit);
    priv->refresh_timer->setInterval(100);
    connect(priv->refresh_timer, &QTimer::timeout, this, &TransactionTableModel::pollTransactionUpdates);
    priv->status_timer->setInterval(100);
    connect(priv->status_timer, &QTimer::timeout, this, &TransactionTableModel::pollStatusUpdates);
    // WalletController can construct this on an RPC thread before moving it
    // to the GUI. Start timers only after it reaches a running event loop.
    QMetaObject::invokeMethod(this, [this] {
        if (m_stopped) return;
        priv->refresh_timer->start();
        pollTransactionUpdates();
    }, Qt::QueuedConnection);
}

TransactionTableModel::~TransactionTableModel()
{
    interrupt();
    delete priv;
}

void TransactionTableModel::interrupt()
{
    if (m_stopped) return;
    m_stopped = true;
    {
        std::lock_guard lock{m_notifications->mutex};
        m_notifications->target = nullptr;
        m_notifications->events.clear();
    }
    unsubscribeFromCoreSignals();
    priv->refresh_timer->stop();
    priv->status_timer->stop();
    // Disconnect first, then drop queued callbacks. A core callback already in
    // flight can still post afterwards, so each GUI entry point also checks the
    // stopped flag. Keep cached rows readable while the owning wallet drains.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    priv->vQueueNotifications.clear();
    priv->pending_transactions.clear();
    priv->transaction_queue.clear();
    priv->queued_transactions.clear();
    priv->inflight_transactions.clear();
    priv->pending_status.clear();
    priv->status_queue.clear();
    priv->queued_status.clear();
    priv->inflight_status.clear();
    priv->completed_status.clear();
    priv->status_apply_queued = false;
    priv->initial_query = {};
    priv->transaction_query = {};
    priv->status_query = {};
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void TransactionTableModel::updateAmountColumnTitle()
{
    if (m_stopped) return;
    columns[Amount] = BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());
    Q_EMIT headerDataChanged(Qt::Horizontal,Amount,Amount);
}

void TransactionTableModel::updateTransaction(const QString &hash, int status, bool showTransaction)
{
    if (m_stopped) return;
    Txid updated = Txid::FromHex(hash.toStdString()).value();
    if (!priv->m_loaded) {
        priv->vQueueNotifications.emplace_back(updated, static_cast<ChangeType>(status), showTransaction);
        return;
    }
    // Do not let an older status query undo an update/delete (or a re-add).
    priv->pending_status.erase(updated);

    // Treat a pending insertion like an existing row: duplicate additions and
    // status updates do not replace it or lose its rescan notification flag.
    // Its status will be fetched lazily after insertion, just like cached rows.
    if (priv->pending_transactions.count(updated) &&
        (status == CT_NEW || (status == CT_UPDATED && showTransaction))) return;

    // Deletions (including updates that hide a transaction) invalidate an
    // outstanding snapshot and apply immediately without touching the wallet.
    priv->pending_transactions.erase(updated);
    if (priv->updateWallet(updated, status, showTransaction)) {
        priv->pending_transactions.emplace(updated, TransactionTablePriv::PendingTransaction{
            ++priv->next_revision, status, fProcessingQueuedTransactions});
        priv->queueTransaction(updated);
        if (!priv->refresh_timer->isActive()) priv->refresh_timer->start();
        if (!priv->transaction_query.valid()) pollTransactionUpdates();
    }
}

void TransactionTableModel::pollTransactionUpdates()
{
    if (m_stopped) return;
    if (!priv->m_loaded) {
        try {
            if (!priv->initial_query.valid()) {
                priv->initial_query = walletModel->requestWalletData([](interfaces::Wallet& wallet) {
                    QList<TransactionRecord> records;
                    for (const auto& wtx : wallet.getWalletTxs()) {
                        if (TransactionRecord::showTransaction()) records.append(TransactionRecord::decomposeTransaction(wtx));
                    }
                    return records;
                });
                return;
            }
            if (priv->initial_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
            auto records{priv->initial_query.get()};
            beginResetModel();
            priv->cachedWallet = std::move(records);
            priv->m_loaded = true;
            endResetModel();
            priv->DispatchNotifications();
        } catch (const std::exception& error) {
            qWarning() << "Initial transaction refresh failed:" << error.what();
            return;
        }
    }
    if (priv->transaction_query.valid()) {
        if (priv->transaction_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            auto snapshots{priv->transaction_query.get()};
            for (size_t i{0}; i < priv->inflight_transactions.size(); ++i) {
                const auto& [hash, request]{priv->inflight_transactions[i]};
                const auto pending{priv->pending_transactions.find(hash)};
                if (pending == priv->pending_transactions.end() || pending->second.revision != request.revision) continue;
                priv->pending_transactions.erase(pending);
                // Retain rescan notification suppression even if the queued
                // processing flag changed while the snapshot was being read.
                QScopedValueRollback<bool> processing{fProcessingQueuedTransactions, request.processing_queued};
                priv->updateWallet(hash, request.status, true, &snapshots[i]);
            }
        } catch (const std::exception& error) {
            qWarning() << "Transaction refresh failed:" << error.what();
            for (const auto& [hash, request] : priv->inflight_transactions) {
                if (const auto pending{priv->pending_transactions.find(hash)};
                    pending != priv->pending_transactions.end() && pending->second.revision == request.revision) {
                    priv->queueTransaction(hash);
                }
            }
            priv->inflight_transactions.clear();
            return; // Retry on the next timer tick, without spinning on errors.
        }
        priv->inflight_transactions.clear();
    }

    // Coalesce repeated notifications for a txid and bound each worker task so
    // transaction bursts also leave room for balance, fee, and claim queries.
    constexpr size_t MAX_TRANSACTION_BATCH{64};
    std::vector<TransactionTablePriv::TransactionRequest> batch;
    batch.reserve(MAX_TRANSACTION_BATCH);
    for (size_t examined{0}; !priv->transaction_queue.empty() && examined < MAX_TRANSACTION_BATCH; ++examined) {
        const auto hash{priv->transaction_queue.front()};
        priv->transaction_queue.pop_front();
        priv->queued_transactions.erase(hash);
        if (const auto pending{priv->pending_transactions.find(hash)}; pending != priv->pending_transactions.end()) {
            batch.emplace_back(hash, pending->second);
        }
    }
    if (batch.empty()) {
        if (priv->transaction_queue.empty()) priv->refresh_timer->stop();
        return;
    }
    try {
        // Only copied requests and backend values cross threads. The model owns
        // the worker; destroying this table never waits for its packaged future.
        priv->transaction_query = walletModel->requestWalletData([batch](interfaces::Wallet& wallet) {
            std::vector<TransactionTablePriv::TransactionSnapshot> snapshots;
            snapshots.reserve(batch.size());
            for (const auto& request : batch) {
                const auto transaction = wallet.getWalletTx(request.first);
                TransactionTablePriv::TransactionSnapshot snapshot;
                snapshot.found = bool(transaction.tx);
                if (snapshot.found) snapshot.records = TransactionRecord::decomposeTransaction(transaction);
                snapshots.push_back(std::move(snapshot));
            }
            return snapshots;
        });
        priv->inflight_transactions = std::move(batch);
    } catch (const std::exception& error) {
        qWarning() << "Unable to request transaction refresh:" << error.what();
        for (const auto& request : batch) priv->queueTransaction(request.first);
    }
}

void TransactionTableModel::pollStatusUpdates()
{
    if (m_stopped || priv->status_apply_queued || priv->status_applying) return;
    QScopedValueRollback<bool> applying{priv->status_applying, true};
    if (priv->status_query.valid()) {
        if (priv->status_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            priv->completed_status = priv->status_query.get();
            priv->next_status = 0;
            priv->next_status_row = 0;
        } catch (const std::exception& error) {
            qWarning() << "Transaction status refresh failed:" << error.what();
            for (const auto& [hash, request] : priv->inflight_status) {
                if (const auto pending{priv->pending_status.find(hash)};
                    pending != priv->pending_status.end() && pending->second.revision == request.revision) {
                    priv->pending_status.erase(pending);
                    priv->queueStatus(hash, walletModel->getLastBlockProcessed(), request.force);
                }
            }
            priv->inflight_status.clear();
        }
    }
    if (!priv->completed_status.empty()) {
        struct ChangedRange {
            int first;
            int last;
            QList<int> roles;
        };
        std::vector<ChangedRange> changed_ranges;
        bool confirmation_details_changed{false};
        const auto current_tip{walletModel->getLastBlockProcessed()};
        constexpr qsizetype MAX_STATUS_ROWS_PER_TURN{128};
        qsizetype remaining_rows{MAX_STATUS_ROWS_PER_TURN};
        while (priv->next_status < priv->inflight_status.size() && remaining_rows > 0) {
            const auto& [hash, request]{priv->inflight_status[priv->next_status]};
            const auto pending{priv->pending_status.find(hash)};
            if (pending == priv->pending_status.end() || pending->second.revision != request.revision) {
                ++priv->next_status;
                priv->next_status_row = 0;
                continue;
            }
            auto lower = std::lower_bound(priv->cachedWallet.begin(), priv->cachedWallet.end(), hash, TxLessThan());
            auto upper = std::upper_bound(lower, priv->cachedWallet.end(), hash, TxLessThan());
            const auto& snapshot{priv->completed_status[priv->next_status]};
            const auto output_count = upper - lower;
            const auto end_output = std::min(output_count, priv->next_status_row + remaining_rows);
            if (snapshot.status && priv->next_status_row < output_count) {
                // Mark the cache with the wallet's actual processed tip,
                // never the newer GUI tip. This also handles same-height
                // reorgs and retries while wallet validation is catching up.
                for (auto row = lower + priv->next_status_row; row != lower + end_output; ++row) {
                    if (request.force || row->status.needsUpdate || row->status.m_cur_block_hash != snapshot.status->block_hash) {
                        const auto previous{row->status};
                        row->updateStatus(*snapshot.status, snapshot.status->block_hash, snapshot.num_blocks, snapshot.block_time);
                        const auto& current{row->status};
                        QList<int> roles;
                        if (previous.status != current.status || previous.depth != current.depth || previous.matures_in != current.matures_in) {
                            roles << Qt::ToolTipRole << TxPlainTextRole << Qt::DecorationRole << RawDecorationRole;
                        }
                        if (previous.status != current.status || previous.countsForBalance != current.countsForBalance) {
                            // DisplayRole is also the proxy's filter role:
                            // conflicts must still enter/leave filtered views.
                            roles << Qt::DisplayRole << Qt::ForegroundRole << StatusRole << ConfirmedRole;
                        }
                        if (previous.sortKey != current.sortKey) roles << Qt::EditRole;
                        if (roles.empty()) continue;
                        // QSortFilterProxyModel checks changed rows even
                        // when the roles exclude its sort/filter roles.
                        // Depth-only details use the existing viewport
                        // repaint lane instead, without proxy comparisons
                        // requesting statuses for unrelated history rows.
                        if (!roles.contains(Qt::EditRole) && !roles.contains(Qt::DisplayRole)) {
                            confirmation_details_changed = true;
                            continue;
                        }
                        const int row_index = row - priv->cachedWallet.begin();
                        if (!changed_ranges.empty() && changed_ranges.back().last + 1 == row_index && changed_ranges.back().roles == roles) {
                            changed_ranges.back().last = row_index;
                        } else {
                            changed_ranges.push_back({row_index, row_index, std::move(roles)});
                        }
                    }
                }
                remaining_rows -= end_output - priv->next_status_row;
                priv->next_status_row = end_output;
                if (priv->next_status_row < output_count) break;
            }
            priv->pending_status.erase(pending);
            ++priv->next_status;
            priv->next_status_row = 0;
            if (!snapshot.status || snapshot.status->block_hash != current_tip) {
                priv->queueStatus(hash, current_tip, request.force);
            }
        }
        // A small batch can touch widely separated hashes. Do not invalidate
        // every intervening history row, making proxies refilter the entire
        // wallet for each batch. Merge only adjacent affected ranges.
        std::sort(changed_ranges.begin(), changed_ranges.end(), [](const auto& first, const auto& second) { return first.first < second.first; });
        for (size_t i{0}; i < changed_ranges.size();) {
            const int first_changed{changed_ranges[i].first};
            const auto roles = changed_ranges[i].roles;
            int last_changed{changed_ranges[i++].last};
            while (i < changed_ranges.size() && changed_ranges[i].first == last_changed + 1 && changed_ranges[i].roles == roles) {
                last_changed = changed_ranges[i++].last;
            }
            Q_EMIT dataChanged(createIndex(first_changed, 0),
                               createIndex(last_changed, columns.size() - 1), roles);
        }
        if (confirmation_details_changed) Q_EMIT confirmationsChanged();
        if (priv->next_status < priv->inflight_status.size()) {
            priv->status_apply_queued = true;
            QMetaObject::invokeMethod(this, [this] {
                priv->status_apply_queued = false;
                pollStatusUpdates();
            }, Qt::QueuedConnection);
            return;
        }
        priv->inflight_status.clear();
        priv->completed_status.clear();
    }

    constexpr size_t MAX_STATUS_BATCH{64};
    std::vector<TransactionTablePriv::StatusEntry> batch;
    batch.reserve(MAX_STATUS_BATCH);
    for (size_t examined{0}; !priv->status_queue.empty() && examined < MAX_STATUS_BATCH; ++examined) {
        const auto hash{priv->status_queue.front()};
        priv->status_queue.pop_front();
        priv->queued_status.erase(hash);
        if (const auto pending{priv->pending_status.find(hash)}; pending != priv->pending_status.end()) {
            batch.emplace_back(hash, pending->second);
        }
    }
    if (batch.empty()) {
        if (priv->status_queue.empty()) priv->status_timer->stop();
        return;
    }
    try {
        priv->status_query = walletModel->requestWalletData([batch](interfaces::Wallet& wallet) {
            std::vector<TransactionTablePriv::StatusSnapshot> snapshots;
            snapshots.reserve(batch.size());
            for (const auto& request : batch) {
                TransactionTablePriv::StatusSnapshot snapshot;
                interfaces::WalletTxStatus status;
                if (wallet.tryGetTxStatus(request.first, status, snapshot.num_blocks, snapshot.block_time)) {
                    snapshot.status = std::move(status);
                }
                snapshots.push_back(std::move(snapshot));
            }
            return snapshots;
        });
        priv->inflight_status = std::move(batch);
    } catch (const std::exception& error) {
        qWarning() << "Unable to request transaction status:" << error.what();
        for (const auto& request : batch) {
            if (priv->queued_status.insert(request.first).second) priv->status_queue.push_back(request.first);
        }
    }
}

void TransactionTableModel::updateConfirmations()
{
    if (m_stopped) return;
    // A full dataChanged range makes QSortFilterProxyModel refilter every row,
    // not just the visible ones. Repaint views to lazily refresh their visible
    // confirmations. Membership-changing conflicts/reorgs arrive as individual
    // CT_UPDATED notifications and refresh even filtered-out rows above.
    Q_EMIT confirmationsChanged();
}

void TransactionTableModel::updateAddressBookLabels(const QString& address)
{
    if (m_stopped) return;
    if (address.isEmpty()) m_all_labels_dirty = true;
    else m_changed_label_addresses.insert(address.toStdString());
    // A newer label change may affect an already examined row. Coalesce the
    // addresses and restart the bounded scan so that no filter misses it.
    m_next_label_row = 0;
    if (m_label_update_pending) return;
    m_label_update_pending = true;
    QMetaObject::invokeMethod(this, &TransactionTableModel::processLabelUpdates, Qt::QueuedConnection);
}

void TransactionTableModel::processLabelUpdates()
{
    if (m_stopped) return;
    // Labels can change during address-book loading or a notification burst.
    // Neither scanning the history nor invalidating its proxy should consume
    // an unbounded GUI turn, even if the changed address has no history rows.
    constexpr int MAX_LABEL_ROWS_PER_TURN{128};
    const int end = std::min(priv->size(), m_next_label_row + MAX_LABEL_ROWS_PER_TURN);
    std::vector<std::pair<int, int>> changed_ranges;
    for (; m_next_label_row < end; ++m_next_label_row) {
        const auto& record = priv->cachedWallet[m_next_label_row];
        if (!record.p2c_domain.empty() || (!m_all_labels_dirty && !m_changed_label_addresses.contains(record.address))) continue;
        if (!changed_ranges.empty() && changed_ranges.back().second == m_next_label_row - 1) {
            changed_ranges.back().second = m_next_label_row;
        } else {
            changed_ranges.emplace_back(m_next_label_row, m_next_label_row);
        }
    }
    for (const auto& [first, last] : changed_ranges) {
        // Custom roles are column-independent, and the filter uses column
        // zero. Do not refilter unrelated rows between sparse matches.
        Q_EMIT dataChanged(createIndex(first, 0),
                           createIndex(last, columns.size() - 1),
                           {Qt::DisplayRole, Qt::EditRole, Qt::ToolTipRole, Qt::ForegroundRole, LabelRole, TxPlainTextRole});
    }
    if (m_next_label_row < priv->size()) {
        QMetaObject::invokeMethod(this, &TransactionTableModel::processLabelUpdates, Qt::QueuedConnection);
    } else {
        m_label_update_pending = false;
        m_all_labels_dirty = false;
        m_changed_label_addresses.clear();
    }
}

int TransactionTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return priv->size();
}

int TransactionTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QString TransactionTableModel::formatTxStatus(const TransactionRecord *wtx) const
{
    QString status;

    switch(wtx->status.status)
    {
    case TransactionStatus::Unconfirmed:
        status = tr("Unconfirmed");
        break;
    case TransactionStatus::Abandoned:
        status = tr("Abandoned");
        break;
    case TransactionStatus::Confirming:
        status = tr("Confirming (%1 of %2 recommended confirmations)").arg(wtx->status.depth).arg(TransactionRecord::RecommendedNumConfirmations);
        break;
    case TransactionStatus::Confirmed:
        status = tr("Confirmed (%1 confirmations)").arg(wtx->status.depth);
        break;
    case TransactionStatus::Conflicted:
        status = tr("Conflicted");
        break;
    case TransactionStatus::Immature:
        status = tr("Immature (%1 confirmations, will be available after %2)").arg(wtx->status.depth).arg(wtx->status.depth + wtx->status.matures_in);
        break;
    case TransactionStatus::NotAccepted:
        status = tr("Generated but not accepted");
        break;
    }

    return status;
}

QString TransactionTableModel::formatTxDate(const TransactionRecord *wtx) const
{
    if(wtx->time)
    {
        return GUIUtil::dateTimeStr(wtx->time);
    }
    return QString();
}

/* Look up address in address book, if found return label (address)
   otherwise just return (address)
 */
QString TransactionTableModel::lookupAddress(const std::string &address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if(!label.isEmpty())
    {
        description += label;
    }
    if(label.isEmpty() || tooltip)
    {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

QString TransactionTableModel::formatTxType(const TransactionRecord *wtx) const
{
    switch(wtx->type)
    {
    case TransactionRecord::RecvWithAddress:
        return tr("Received with");
    case TransactionRecord::RecvFromOther:
        return tr("Received from");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
        return tr("Sent to");
    case TransactionRecord::Generated:
        return tr("Mined");
    default:
        return QString();
    }
}

QVariant TransactionTableModel::txAddressDecoration(const TransactionRecord *wtx) const
{
    switch(wtx->type)
    {
    case TransactionRecord::Generated:
        return QIcon(":/icons/tx_mined");
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::RecvFromOther:
        return QIcon(":/icons/tx_input");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
        return QIcon(":/icons/tx_output");
    default:
        return QIcon(":/icons/tx_inout");
    }
}

QString TransactionTableModel::formatTxToAddress(const TransactionRecord *wtx, bool tooltip) const
{
    if (!wtx->p2c_domain.empty()) {
        return tr("P2C: %1").arg(QString::fromStdString(wtx->p2c_domain));
    }
    switch(wtx->type)
    {
    case TransactionRecord::RecvFromOther:
        return QString::fromStdString(wtx->address);
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
        return lookupAddress(wtx->address, tooltip);
    case TransactionRecord::SendToOther:
        return QString::fromStdString(wtx->address);
    default:
        return tr("(n/a)");
    }
}

QVariant TransactionTableModel::addressColor(const TransactionRecord *wtx) const
{
    // Show addresses without label in a less visible color
    switch(wtx->type)
    {
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
        {
        QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(wtx->address));
        if(label.isEmpty())
            return COLOR_BAREADDRESS;
        } break;
    default:
        break;
    }
    return QVariant();
}

QString TransactionTableModel::formatTxAmount(const TransactionRecord *wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->credit + wtx->debit, false, separators);
    if(showUnconfirmed)
    {
        if(!wtx->status.countsForBalance)
        {
            str = QString("[") + str + QString("]");
        }
    }
    return QString(str);
}

QVariant TransactionTableModel::txStatusDecoration(const TransactionRecord *wtx) const
{
    switch(wtx->status.status)
    {
    case TransactionStatus::Unconfirmed:
        return QIcon(":/icons/transaction_0");
    case TransactionStatus::Abandoned:
        return QIcon(":/icons/transaction_abandoned");
    case TransactionStatus::Confirming:
        switch(wtx->status.depth)
        {
        case 1: return QIcon(":/icons/transaction_1");
        case 2: return QIcon(":/icons/transaction_2");
        case 3: return QIcon(":/icons/transaction_3");
        case 4: return QIcon(":/icons/transaction_4");
        default: return QIcon(":/icons/transaction_5");
        };
    case TransactionStatus::Confirmed:
        return QIcon(":/icons/transaction_confirmed");
    case TransactionStatus::Conflicted:
        return QIcon(":/icons/transaction_conflicted");
    case TransactionStatus::Immature: {
        int total = wtx->status.depth + wtx->status.matures_in;
        int part = (wtx->status.depth * 4 / total) + 1;
        return QIcon(QString(":/icons/transaction_%1").arg(part));
        }
    case TransactionStatus::NotAccepted:
        return QIcon(":/icons/transaction_0");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

QString TransactionTableModel::formatTooltip(const TransactionRecord *rec) const
{
    QString tooltip = formatTxStatus(rec) + QString("\n") + formatTxType(rec);
    if(rec->type==TransactionRecord::RecvFromOther || rec->type==TransactionRecord::SendToOther ||
       rec->type==TransactionRecord::SendToAddress || rec->type==TransactionRecord::RecvWithAddress)
    {
        tooltip += QString(" ") + formatTxToAddress(rec, true);
    }
    return tooltip;
}

QVariant TransactionTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= priv->size() ||
        index.column() < 0 || index.column() >= columns.size())
        return QVariant();
    // QList storage moves when a transaction is inserted or removed. Qt keeps
    // persistent index row numbers current, not pointers into that storage.
    const TransactionRecord* rec = &priv->cachedWallet[index.row()];

    const auto column = static_cast<ColumnIndex>(index.column());
    switch (role) {
    case RawDecorationRole:
        switch (column) {
        case Status:
            return txStatusDecoration(rec);
        case Date: return {};
        case Type: return {};
        case ToAddress:
            return txAddressDecoration(rec);
        case Amount: return {};
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    case Qt::DecorationRole:
    {
        QIcon icon = qvariant_cast<QIcon>(index.data(RawDecorationRole));
        return platformStyle->TextColorIcon(icon);
    }
    case Qt::DisplayRole:
        switch (column) {
        case Status: return {};
        case Date:
            return formatTxDate(rec);
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, false);
        case Amount:
            return formatTxAmount(rec, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    case Qt::EditRole:
        // Edit role is used for sorting, so return the unformatted values
        switch (column) {
        case Status:
            return QString::fromStdString(rec->status.sortKey);
        case Date:
            return QString::fromStdString(strprintf("%020s-%s", rec->time, rec->status.sortKey));
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, true);
        case Amount:
            return qint64(rec->credit + rec->debit);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    case Qt::ToolTipRole:
        return formatTooltip(rec);
    case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
    case Qt::ForegroundRole:
        // Use the "danger" color for abandoned transactions
        if(rec->status.status == TransactionStatus::Abandoned)
        {
            return COLOR_TX_STATUS_DANGER;
        }
        // Non-confirmed (but not immature) as transactions are grey
        if(!rec->status.countsForBalance && rec->status.status != TransactionStatus::Immature)
        {
            return COLOR_UNCONFIRMED;
        }
        if(index.column() == Amount && (rec->credit+rec->debit) < 0)
        {
            return COLOR_NEGATIVE;
        }
        if(index.column() == ToAddress)
        {
            return addressColor(rec);
        }
        break;
    case TypeRole:
        return rec->type;
    case DateRole:
        return QDateTime::fromSecsSinceEpoch(rec->time);
    case AddressRole:
        return QString::fromStdString(rec->address);
    case LabelRole:
        if (!rec->p2c_domain.empty()) return formatTxToAddress(rec, false);
        return walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));
    case AmountRole:
        return qint64(rec->credit + rec->debit);
    case TxHashRole:
        return rec->getTxHash();
    case TxPlainTextRole:
        {
            QString details;
            QDateTime date = QDateTime::fromSecsSinceEpoch(rec->time);
            QString txLabel = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));

            details.append(date.toString("M/d/yy HH:mm"));
            details.append(" ");
            details.append(formatTxStatus(rec));
            details.append(". ");
            if(!formatTxType(rec).isEmpty()) {
                details.append(formatTxType(rec));
                details.append(" ");
            }
            if (!rec->p2c_domain.empty()) {
                details.append(formatTxToAddress(rec, false));
                details.append(" ");
            } else if(!rec->address.empty()) {
                if(txLabel.isEmpty())
                    details.append(tr("(no label)") + " ");
                else {
                    details.append("(");
                    details.append(txLabel);
                    details.append(") ");
                }
                details.append(QString::fromStdString(rec->address));
                details.append(" ");
            }
            details.append(formatTxAmount(rec, false, BitcoinUnits::SeparatorStyle::NEVER));
            return details;
        }
    case ConfirmedRole:
        return rec->status.status == TransactionStatus::Status::Confirming || rec->status.status == TransactionStatus::Status::Confirmed;
    case FormattedAmountRole:
        // Used for copy/export, so don't include separators
        return formatTxAmount(rec, false, BitcoinUnits::SeparatorStyle::NEVER);
    case StatusRole:
        return rec->status.status;
    }
    return QVariant();
}

std::future<QString> TransactionTableModel::requestTxDescription(const QModelIndex& source_index) const
{
    if (m_stopped || source_index.model() != this || source_index.row() < 0 || source_index.row() >= priv->size()) {
        std::promise<QString> result;
        result.set_value({});
        return result.get_future();
    }
    // Capture the record and display unit before an explicit action can wait:
    // proxy sorting, row removal, or insertion may invalidate its old index.
    return walletModel->requestWalletData([node = &walletModel->node(), record = priv->cachedWallet[source_index.row()],
                                          unit = walletModel->getOptionsModel()->getDisplayUnit()](interfaces::Wallet& wallet) mutable {
        return TransactionDesc::toHTML(*node, wallet, &record, unit);
    });
}

std::future<QString> TransactionTableModel::requestTxHex(const QModelIndex& source_index) const
{
    if (m_stopped || source_index.model() != this || source_index.row() < 0 || source_index.row() >= priv->size()) {
        std::promise<QString> result;
        result.set_value({});
        return result.get_future();
    }
    return walletModel->requestWalletData([hash = priv->cachedWallet[source_index.row()].hash](interfaces::Wallet& wallet) {
        const auto tx{wallet.getTx(hash)};
        return tx ? QString::fromStdString(EncodeHexTx(*tx)) : QString{};
    });
}

QVariant TransactionTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return columns[section];
        }
        else if (role == Qt::TextAlignmentRole)
        {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole)
        {
            switch(section)
            {
            case Status:
                return tr("Transaction status. Hover over this field to show number of confirmations.");
            case Date:
                return tr("Date and time that the transaction was received.");
            case Type:
                return tr("Type of transaction.");
            case ToAddress:
                return tr("User-defined intent/purpose of the transaction, or the P2C domain.");
            case Amount:
                return tr("Amount removed from or added to balance.");
            }
        }
    }
    return QVariant();
}

QModelIndex TransactionTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    TransactionRecord* data = priv->index(walletModel->getLastBlockProcessed(), row);
    if(data)
    {
        return createIndex(row, column);
    }
    return QModelIndex();
}

QModelIndexList TransactionTableModel::indexesForTransaction(const Txid& txid) const
{
    const auto begin = priv->cachedWallet.cbegin();
    const auto [first, last] = std::equal_range(begin, priv->cachedWallet.cend(), txid, TxLessThan{});
    QModelIndexList indexes;
    indexes.reserve(last - first);
    for (auto record = first; record != last; ++record) {
        indexes.append(createIndex(record - begin, Status));
    }
    return indexes;
}

void TransactionTableModel::updateDisplayUnit()
{
    if (m_stopped) return;
    // emit dataChanged to update Amount column with the current unit
    updateAmountColumnTitle();
    Q_EMIT dataChanged(index(0, Amount), index(priv->size()-1, Amount));
}

void TransactionTablePriv::NotifyTransactionChanged(const Txid& hash, ChangeType status)
{
    if (parent->m_stopped) return;
    // Find transaction in wallet
    // Determine whether to show transaction or not (determine this here so that no relocking is needed in GUI thread)
    bool showTransaction = TransactionRecord::showTransaction();

    TransactionNotification notification(hash, status, showTransaction);

    if (!m_loaded || m_loading || dispatch_pending || !vQueueNotifications.empty())
    {
        vQueueNotifications.push_back(notification);
        DispatchNotifications();
        return;
    }
    notification.invoke(parent);
}

void TransactionTablePriv::DispatchNotifications()
{
    if (parent->m_stopped || !m_loaded || m_loading || dispatch_pending) return;
    dispatch_pending = true;
    constexpr size_t MAX_NOTIFICATION_BATCH{64};
    for (size_t count{0}; !vQueueNotifications.empty() && count < MAX_NOTIFICATION_BATCH; ++count) {
        // Preserve the last-ten balloon policy across separate event turns.
        parent->setProcessingQueuedTransactions(vQueueNotifications.size() > 10);
        auto notification = std::move(vQueueNotifications.front());
        vQueueNotifications.pop_front();
        notification.invoke(parent);
    }
    parent->setProcessingQueuedTransactions(false);
    if (!vQueueNotifications.empty()) {
        QMetaObject::invokeMethod(parent, [this] {
            dispatch_pending = false;
            DispatchNotifications();
        }, Qt::QueuedConnection);
    } else dispatch_pending = false;
}

void TransactionTableModel::processBackendNotifications()
{
    if (m_stopped) return;
    constexpr size_t MAX_NOTIFICATION_BATCH{64};
    std::deque<TransactionNotificationQueue::Event> events;
    {
        std::lock_guard lock{m_notifications->mutex};
        for (size_t count{0}; !m_notifications->events.empty() && count < MAX_NOTIFICATION_BATCH; ++count) {
            events.push_back(std::move(m_notifications->events.front()));
            m_notifications->events.pop_front();
        }
    }
    for (const auto& event : events) {
        if (m_stopped) return;
        if (const auto* transaction = std::get_if<std::pair<Txid, ChangeType>>(&event)) {
            priv->NotifyTransactionChanged(transaction->first, transaction->second);
        } else {
            priv->m_loading = std::get<int>(event) < 100;
            priv->DispatchNotifications();
        }
    }
    {
        std::lock_guard lock{m_notifications->mutex};
        if (!m_notifications->target) return;
        // Keep scheduled true throughout processing: even a nested event loop
        // must not deliver newer core events before this batch has finished.
        if (m_notifications->events.empty()) m_notifications->scheduled = false;
        else QMetaObject::invokeMethod(this, "processBackendNotifications", Qt::QueuedConnection);
    }
}

void TransactionTableModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_transaction_changed = walletModel->wallet().handleTransactionChanged([queue = m_notifications](const Txid& hash, ChangeType status) {
        queue->push(std::pair{hash, status});
    });
    m_handler_show_progress = walletModel->wallet().handleShowProgress([queue = m_notifications](const std::string&, int progress) {
        queue->push(progress);
    });
}

void TransactionTableModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
}
