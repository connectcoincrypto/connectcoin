// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/recentrequeststablemodel.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <clientversion.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <streams.h>
#include <util/string.h>

#include <utility>
#include <memory>
#include <map>
#include <set>
#include <vector>

#include <QLatin1Char>
#include <QLatin1String>
#include <QDebug>
#include <QPointer>
#include <QTimer>

using util::ToString;

namespace {
using RemovalRanges = std::vector<std::pair<int, int>>;

RemovalRanges FindRemovedRequestRanges(const QList<RecentRequestEntry>& entries, const std::set<int64_t>& ids)
{
    RemovalRanges ranges;
    for (int row{0}; row < entries.size(); ++row) {
        if (!ids.contains(entries[row].id)) continue;
        if (!ranges.empty() && ranges.back().second + 1 == row) ranges.back().second = row;
        else ranges.emplace_back(row, row);
    }
    return ranges;
}
} // namespace

RecentRequestsTableModel::RecentRequestsTableModel(WalletModel *parent) :
    QAbstractTableModel(parent), walletModel(parent)
{
    // Read and deserialize history without taking wallet/database locks on
    // the GUI thread. Explicit writes wait for this snapshot's ID watermark.
    m_snapshot = parent->requestWalletData([](interfaces::Wallet& wallet) {
        Snapshot snapshot;
        for (const auto& request : wallet.getAddressReceiveRequests()) {
            SpanReader stream{MakeByteSpan(request)};
            RecentRequestEntry entry;
            stream >> entry;
            if (entry.id == 0) continue;
            snapshot.max_id = std::max(snapshot.max_id, entry.id);
            snapshot.entries.append(std::move(entry));
        }
        std::sort(snapshot.entries.begin(), snapshot.entries.end(), RecentRequestEntryLessThan(Date, Qt::DescendingOrder));
        return snapshot;
    });
    m_snapshot_timer = new QTimer(this);
    m_snapshot_timer->setInterval(50);
    connect(m_snapshot_timer, &QTimer::timeout, this, [this] {
        if (!m_snapshot.valid() || m_snapshot.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        m_snapshot_timer->stop();
        try {
            applySnapshot(m_snapshot.get());
        } catch (const std::exception& error) {
            qWarning() << "Receive request history failed:" << error.what();
        }
    });
    QMetaObject::invokeMethod(m_snapshot_timer, [timer = m_snapshot_timer] { timer->start(); }, Qt::QueuedConnection);
    m_sort_timer = new QTimer(this);
    m_sort_timer->setObjectName("receiveRequestSortRetry");
    m_sort_timer->setInterval(50);
    connect(m_sort_timer, &QTimer::timeout, this, &RecentRequestsTableModel::pollDeferredSort);

    /* These columns must match the indices in the ColumnIndex enumeration */
    columns << tr("Date") << tr("Label") << tr("Message") << getAmountTitle();

    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RecentRequestsTableModel::updateDisplayUnit);
}

RecentRequestsTableModel::~RecentRequestsTableModel() = default;

void RecentRequestsTableModel::applySnapshot(Snapshot snapshot)
{
    m_snapshot_timer->stop();
    // A different header may have been selected while the initial read was
    // running. Re-sort its value snapshot asynchronously too; a timer callback
    // must not suddenly perform a full-history sort on the GUI thread.
    if (m_sort_column != snapshot.sort_column || m_sort_order != snapshot.sort_order) {
        m_snapshot = walletModel->requestWalletData(
            [snapshot = std::move(snapshot), column = m_sort_column, order = m_sort_order](interfaces::Wallet&) mutable {
                std::sort(snapshot.entries.begin(), snapshot.entries.end(), RecentRequestEntryLessThan(column, order));
                snapshot.sort_column = column;
                snapshot.sort_order = order;
                return std::move(snapshot);
            });
        m_snapshot_timer->start();
        return;
    }
    beginResetModel();
    list = std::move(snapshot.entries);
    ++m_revision;
    nReceiveRequestsMaxId = snapshot.max_id;
    m_ready = true;
    endResetModel();
    Q_EMIT ready();
}

void RecentRequestsTableModel::ensureReady()
{
    while (!m_ready) {
        if (!m_snapshot.valid()) throw std::runtime_error("Receive request history is unavailable");
        m_snapshot_timer->stop();
        applySnapshot(GUIUtil::WaitForBackendTask(std::move(m_snapshot)));
    }
}

int RecentRequestsTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return list.length();
}

int RecentRequestsTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant RecentRequestsTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid() || index.row() >= list.length())
        return QVariant();

    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        const RecentRequestEntry *rec = &list[index.row()];
        switch(index.column())
        {
        case Date:
            return GUIUtil::dateTimeStr(rec->date);
        case Label:
            if(rec->recipient.label.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no label)");
            }
            else
            {
                return rec->recipient.label;
            }
        case Message:
            if(rec->recipient.message.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no message)");
            }
            else
            {
                return rec->recipient.message;
            }
        case Amount:
            if (rec->recipient.amount == 0 && role == Qt::DisplayRole)
                return tr("(no amount requested)");
            else if (role == Qt::EditRole)
                return BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), rec->recipient.amount, false, BitcoinUnits::SeparatorStyle::NEVER);
            else
                return BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), rec->recipient.amount);
        }
    }
    else if (role == Qt::TextAlignmentRole)
    {
        if (index.column() == Amount)
            return (int)(Qt::AlignRight|Qt::AlignVCenter);
    }
    return QVariant();
}

bool RecentRequestsTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    return true;
}

QVariant RecentRequestsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
        {
            return columns[section];
        }
    }
    return QVariant();
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void RecentRequestsTableModel::updateAmountColumnTitle()
{
    columns[Amount] = getAmountTitle();
    Q_EMIT headerDataChanged(Qt::Horizontal,Amount,Amount);
}

/** Gets title for amount column including current display unit if optionsModel reference available. */
QString RecentRequestsTableModel::getAmountTitle()
{
    if (!walletModel->getOptionsModel()) return {};
    return tr("Requested") +
           QLatin1String(" (") +
           BitcoinUnits::shortName(this->walletModel->getOptionsModel()->getDisplayUnit()) +
           QLatin1Char(')');
}

QModelIndex RecentRequestsTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return createIndex(row, column);
}

bool RecentRequestsTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<RecentRequestsTableModel> guard{this};
    Q_UNUSED(parent);

    if (count > 0 && row >= 0 && row <= list.size() && count <= list.size() - row)
    {
        struct Removal {
            std::shared_ptr<const std::set<int64_t>> ids;
            RemovalRanges ranges;
        };
        auto revision = m_revision;
        // Copying QList is constant-time. Selection extraction, persistence
        // and matching all run on the worker, rather than doing a GUI scan
        // (and shifting the list) once for every selected request.
        auto removed = GUIUtil::WaitForBackendTask(walletModel->requestWalletData([entries = list, row, count](interfaces::Wallet& wallet) {
            auto ids = std::make_shared<std::set<int64_t>>();
            for (int index = row; index < row + count; ++index) {
                const auto& request = entries[index];
                if (!wallet.setAddressReceiveRequest(DecodeDestination(request.recipient.address.toStdString()), ToString(request.id), "")) break;
                ids->insert(request.id);
            }
            auto ranges = FindRemovedRequestRanges(entries, *ids);
            return Removal{std::move(ids), std::move(ranges)};
        }));
        if (!guard || removed.ids->empty()) return false;
        // Sorting or another edit can run during a responsive wait. Rebase
        // only the successfully persisted deletions onto the newest order,
        // preserving newly added requests and partial database failures.
        for (int retry{0}; revision != m_revision && retry < 3; ++retry) {
            revision = m_revision;
            removed.ranges = GUIUtil::WaitForBackendTask(walletModel->requestWalletData(
                [entries = list, ids = removed.ids](interfaces::Wallet&) {
                    return FindRemovedRequestRanges(entries, *ids);
                }));
            if (!guard) return false;
        }
        if (revision != m_revision) {
            // Pathological edits during all three waits must not keep this
            // action alive forever or leave already-erased database records
            // displayed. One current-state scan is the GUI coherency fallback.
            removed.ranges = FindRemovedRequestRanges(list, *removed.ids);
        }
        // The usual contiguous selection needs one model operation/list shift.
        // Reverse order also preserves indexes of surviving requests when a
        // sort during the wait has split the deleted IDs into several ranges.
        for (auto range = removed.ranges.rbegin(); range != removed.ranges.rend(); ++range) {
            beginRemoveRows({}, range->first, range->second);
            list.remove(range->first, range->second - range->first + 1);
            ++m_revision;
            endRemoveRows();
        }
        return removed.ids->size() == static_cast<size_t>(count);
    } else {
        return false;
    }
}

Qt::ItemFlags RecentRequestsTableModel::flags(const QModelIndex &index) const
{
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

// called when adding a request from the GUI
void RecentRequestsTableModel::addNewRequest(const SendCoinsRecipient &recipient)
{
    GUIUtil::BackendOperationGuard operation;
    ensureReady();
    RecentRequestEntry newEntry;
    newEntry.id = ++nReceiveRequestsMaxId;
    newEntry.date = QDateTime::currentDateTime();
    newEntry.recipient = recipient;

    if (!GUIUtil::WaitForBackendTask(walletModel->requestWalletData([newEntry](interfaces::Wallet& wallet) {
        DataStream stream{};
        stream << newEntry;
        return wallet.setAddressReceiveRequest(DecodeDestination(newEntry.recipient.address.toStdString()), ToString(newEntry.id), stream.str());
    })))
        return;

    addNewRequest(newEntry);
}

// called from ctor when loading from wallet
void RecentRequestsTableModel::addNewRequest(const std::string &recipient)
{
    SpanReader ss{MakeByteSpan(recipient)};

    RecentRequestEntry entry;
    ss >> entry;

    if (entry.id == 0) // should not happen
        return;

    if (entry.id > nReceiveRequestsMaxId)
        nReceiveRequestsMaxId = entry.id;

    addNewRequest(entry);
}

// actually add to table in GUI
void RecentRequestsTableModel::addNewRequest(RecentRequestEntry &recipient)
{
    beginInsertRows(QModelIndex(), 0, 0);
    list.prepend(recipient);
    ++m_revision;
    endInsertRows();
}

void RecentRequestsTableModel::sort(int column, Qt::SortOrder order)
{
    GUIUtil::BackendOperationGuard operation;
    const QPointer<RecentRequestsTableModel> guard{this};
    m_sort_column = column;
    m_sort_order = order;
    const auto request = ++m_sort_request;
    m_sort_timer->stop();
    m_sort_query = {}; // Packaged-task futures never block when discarded.
    if (list.size() < 2) return;
    SortSnapshot sorted;
    uint64_t revision{m_revision};
    for (int retry{0}; retry < 3; ++retry) {
        revision = m_revision;
        sorted = GUIUtil::WaitForBackendTask(walletModel->requestWalletData(
            [column, order, entries = list](interfaces::Wallet&) { return prepareSort(entries, column, order); }));
        if (!guard || request != m_sort_request) return;
        if (revision == m_revision) break;
    }
    // Continual edits should neither extend a modal wait forever nor move a
    // full-history sort back to the GUI. Keep one asynchronous latest-state
    // retry; the last selected header wins and stale snapshots are discarded.
    if (revision != m_revision) {
        startDeferredSort();
        return;
    }
    applySort(std::move(sorted));
}

RecentRequestsTableModel::SortSnapshot RecentRequestsTableModel::prepareSort(QList<RecentRequestEntry> entries, int column, Qt::SortOrder order)
{
    std::sort(entries.begin(), entries.end(), RecentRequestEntryLessThan(column, order));
    SortSnapshot sorted{std::move(entries), {}};
    for (int row{0}; row < sorted.entries.size(); ++row) sorted.positions.emplace(sorted.entries[row].id, row);
    return sorted;
}

void RecentRequestsTableModel::startDeferredSort()
{
    m_sort_revision = m_revision;
    m_sort_query = walletModel->requestWalletData(
        [entries = list, column = m_sort_column, order = m_sort_order](interfaces::Wallet&) {
            return prepareSort(entries, column, order);
        });
    m_sort_timer->start();
}

void RecentRequestsTableModel::pollDeferredSort()
{
    if (!m_sort_query.valid()) { m_sort_timer->stop(); return; }
    if (m_sort_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
    m_sort_timer->stop();
    try {
        auto sorted = m_sort_query.get();
        if (m_sort_revision != m_revision) startDeferredSort();
        else applySort(std::move(sorted));
    } catch (const std::exception& error) {
        qWarning() << "Receive request sorting failed:" << error.what();
    }
}

void RecentRequestsTableModel::applySort(SortSnapshot sorted)
{
    Q_EMIT layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const auto before = persistentIndexList();
    QModelIndexList after;
    after.reserve(before.size());
    for (const auto& previous : before) {
        const auto position = sorted.positions.find(list.at(previous.row()).id);
        after.append(position == sorted.positions.end() ? QModelIndex{} : index(position->second, previous.column()));
    }
    list.swap(sorted.entries);
    ++m_revision;
    changePersistentIndexList(before, after);
    Q_EMIT layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void RecentRequestsTableModel::updateDisplayUnit()
{
    updateAmountColumnTitle();
}

bool RecentRequestEntryLessThan::operator()(const RecentRequestEntry& left, const RecentRequestEntry& right) const
{
    const RecentRequestEntry* pLeft = &left;
    const RecentRequestEntry* pRight = &right;
    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch(column)
    {
    case RecentRequestsTableModel::Date:
        return pLeft->date.toSecsSinceEpoch() < pRight->date.toSecsSinceEpoch();
    case RecentRequestsTableModel::Label:
        return pLeft->recipient.label < pRight->recipient.label;
    case RecentRequestsTableModel::Message:
        return pLeft->recipient.message < pRight->recipient.message;
    case RecentRequestsTableModel::Amount:
        return pLeft->recipient.amount < pRight->recipient.amount;
    default:
        return pLeft->id < pRight->id;
    }
}
