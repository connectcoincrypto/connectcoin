// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bantablemodel.h>
#include <qt/guiutil.h>

#include <interfaces/node.h>
#include <net_types.h>

#include <utility>

#include <QDateTime>
#include <QDebug>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QVariant>
#include <QTimer>

bool BannedNodeLessThan::operator()(const CCombinedBan& left, const CCombinedBan& right) const
{
    const CCombinedBan* pLeft = &left;
    const CCombinedBan* pRight = &right;

    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch (static_cast<BanTableModel::ColumnIndex>(column)) {
    case BanTableModel::Address:
        return pLeft->subnet.ToString().compare(pRight->subnet.ToString()) < 0;
    case BanTableModel::Bantime:
        return pLeft->banEntry.nBanUntil < pRight->banEntry.nBanUntil;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

// private implementation
class BanTablePriv
{
public:
    /** Local cache of peer information */
    QList<CCombinedBan> cachedBanlist;
    /** Column to sort nodes by (default to unsorted) */
    int sortColumn{-1};
    /** Order (ascending or descending) to sort nodes by */
    Qt::SortOrder sortOrder{Qt::AscendingOrder};

    int size() const
    {
        return cachedBanlist.size();
    }

    CCombinedBan *index(int idx)
    {
        if (idx >= 0 && idx < cachedBanlist.size())
            return &cachedBanlist[idx];

        return nullptr;
    }
};

BanTableModel::BanTableModel(interfaces::Node& node, QObject* parent) :
    QAbstractTableModel(parent),
    m_node(node)
{
    columns << tr("IP/Netmask") << tr("Banned Until");
    priv.reset(new BanTablePriv());

    m_worker.Start(1);
    m_result_timer = new QTimer(this);
    m_result_timer->setInterval(50);
    connect(m_result_timer, &QTimer::timeout, this, &BanTableModel::pollRefresh);

    // load initial data
    refresh();
}

BanTableModel::~BanTableModel()
{
    interrupt();
    m_worker.Stop();
}

void BanTableModel::stop()
{
    if (m_worker_stopped) return;
    interrupt();
    m_worker_stopped = true;
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [worker = &m_worker] { worker->Stop(); }));
}

void BanTableModel::interrupt()
{
    m_stopped = true;
    m_result_timer->stop();
    m_worker.Interrupt();
}

int BanTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return priv->size();
}

int BanTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant BanTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();

    CCombinedBan *rec = static_cast<CCombinedBan*>(index.internalPointer());

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Address:
            return QString::fromStdString(rec->subnet.ToString());
        case Bantime:
            QDateTime date = QDateTime::fromMSecsSinceEpoch(0);
            date = date.addSecs(rec->banEntry.nBanUntil);
            return QLocale::system().toString(date, QLocale::LongFormat);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    }

    return QVariant();
}

QVariant BanTableModel::headerData(int section, Qt::Orientation orientation, int role) const
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

Qt::ItemFlags BanTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex BanTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    CCombinedBan *data = priv->index(row);

    if (data)
        return createIndex(row, column, data);
    return QModelIndex();
}

void BanTableModel::refresh()
{
    if (m_stopped) return;
    m_refresh_requested = true;
    pollRefresh();
}

void BanTableModel::pollRefresh()
{
    if (m_stopped) return;
    for (auto query = m_unban_queries.begin(); query != m_unban_queries.end();) {
        if (query->wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++query;
            continue;
        }
        try {
            if (query->get()) m_refresh_requested = true;
        } catch (const std::exception& error) {
            qWarning() << "Unban failed:" << error.what();
        }
        query = m_unban_queries.erase(query);
    }
    if (m_query.valid()) {
        if (m_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            auto bans = m_query.get();
            // A newer sort/unban request invalidates the old snapshot.
            if (!m_refresh_requested) {
                beginResetModel();
                priv->cachedBanlist = std::move(bans);
                endResetModel();
            }
        } catch (const std::exception& error) {
            qWarning() << "Ban list refresh failed:" << error.what();
        }
    }
    if (m_stopped || m_query.valid()) return;
    if (m_refresh_requested) {
        m_refresh_requested = false;
        auto result = m_worker.Submit([node = &m_node, column = priv->sortColumn, order = priv->sortOrder] {
            banmap_t bans;
            node->getBanned(bans);
            QList<CCombinedBan> rows;
            rows.reserve(bans.size());
            for (const auto& [subnet, entry] : bans) rows.append(CCombinedBan{subnet, entry});
            if (column >= 0) std::stable_sort(rows.begin(), rows.end(), BannedNodeLessThan(column, order));
            return rows;
        });
        if (result) m_query = std::move(*result);
    }
    if (m_query.valid() || !m_unban_queries.empty()) m_result_timer->start();
    else m_result_timer->stop();
}

void BanTableModel::sort(int column, Qt::SortOrder order)
{
    priv->sortColumn = column;
    priv->sortOrder = order;
    refresh();
}

bool BanTableModel::shouldShow()
{
    return priv->size() > 0;
}

bool BanTableModel::unban(const QModelIndex& index)
{
    if (m_stopped || !index.isValid() || index.model() != this) return false;
    CCombinedBan* ban{static_cast<CCombinedBan*>(index.internalPointer())};
    if (!ban) return false;
    auto result = m_worker.Submit([node = &m_node, subnet = ban->subnet] { return node->unban(subnet); });
    if (!result) return false;
    m_unban_queries.push_back(std::move(*result));
    m_result_timer->start();
    return true;
}
