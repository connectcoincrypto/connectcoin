// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/peertablemodel.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <interfaces/node.h>

#include <utility>

#include <QList>
#include <QDebug>
#include <QTimer>

PeerTableModel::PeerTableModel(interfaces::Node& node, QObject* parent)
    : QAbstractTableModel(parent),
      m_node(node)
{
    // set up timer for auto refresh
    timer = new QTimer(this);
    timer->setObjectName("peerRefreshTimer");
    connect(timer, &QTimer::timeout, this, &PeerTableModel::refresh);
    timer->setInterval(MODEL_UPDATE_DELAY);

    m_worker.Start(1);
    m_result_timer = new QTimer(this);
    m_result_timer->setObjectName("peerRefreshResultTimer");
    m_result_timer->setInterval(50);
    connect(m_result_timer, &QTimer::timeout, this, &PeerTableModel::pollRefresh);

    // load initial data
    refresh();
}

PeerTableModel::~PeerTableModel()
{
    interrupt();
    m_worker.Stop();
}

void PeerTableModel::stop()
{
    if (m_worker_stopped) return;
    interrupt();
    m_worker_stopped = true;
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [worker = &m_worker] { worker->Stop(); }));
}

void PeerTableModel::interrupt()
{
    m_stopped = true;
    timer->stop();
    m_result_timer->stop();
    m_worker.Interrupt();
}

void PeerTableModel::startAutoRefresh()
{
    if (m_stopped || timer->isActive()) return;
    timer->start();
    refresh();
}

void PeerTableModel::stopAutoRefresh()
{
    timer->stop();
    m_refresh_requested = false;
    // Keep an in-flight snapshot for the next show, but do not publish/sort it
    // or schedule replacement work while the peer table is not visible.
    m_result_timer->stop();
}

int PeerTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_peers_data.size();
}

int PeerTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant PeerTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.model() != this || index.row() < 0 || index.row() >= m_peers_data.size())
        return QVariant();

    // Snapshots replace the backing QList, and removals move its elements.
    // A persistent model index follows its row, not an address in the old list.
    const auto* rec = &m_peers_data.at(index.row());

    const auto column = static_cast<ColumnIndex>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case NetNodeId:
            return (qint64)rec->nodeStats.nodeid;
        case Age:
            return GUIUtil::FormatPeerAge(rec->nodeStats.m_connected);
        case Address:
            return QString::fromStdString(rec->nodeStats.m_addr_name);
        case Direction:
            return QString(rec->nodeStats.fInbound ?
                               //: An Inbound Connection from a Peer.
                               tr("Inbound") :
                               //: An Outbound Connection to a Peer.
                               tr("Outbound"));
        case ConnectionType:
            return GUIUtil::ConnectionTypeToQString(rec->nodeStats.m_conn_type, /*prepend_direction=*/false);
        case Network:
            return GUIUtil::NetworkToQString(rec->nodeStats.m_network);
        case Ping:
            return GUIUtil::formatPingTime(rec->nodeStats.m_min_ping_time);
        case Sent:
            return GUIUtil::formatBytes(rec->nodeStats.nSendBytes);
        case Received:
            return GUIUtil::formatBytes(rec->nodeStats.nRecvBytes);
        case Subversion:
            return QString::fromStdString(rec->nodeStats.cleanSubVer);
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case NetNodeId:
        case Age:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        case Address:
            return {};
        case Direction:
        case ConnectionType:
        case Network:
            return QVariant(Qt::AlignCenter);
        case Ping:
        case Sent:
        case Received:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        case Subversion:
            return {};
        } // no default case, so the compiler can warn about missing cases
        assert(false);
    } else if (role == StatsRole) {
        return QVariant::fromValue(const_cast<CNodeCombinedStats*>(rec));
    }

    return QVariant();
}

QVariant PeerTableModel::headerData(int section, Qt::Orientation orientation, int role) const
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

Qt::ItemFlags PeerTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    return retval;
}

QModelIndex PeerTableModel::index(int row, int column, const QModelIndex& parent) const
{
    if (parent.isValid()) return {};

    if (0 <= row && row < rowCount() && 0 <= column && column < columnCount()) {
        return createIndex(row, column);
    }

    return QModelIndex();
}

void PeerTableModel::refresh()
{
    if (m_stopped) return;
    m_refresh_requested = true;
    m_result_timer->start();
    pollRefresh();
}

void PeerTableModel::pollRefresh()
{
    if (m_stopped) return;
    if (m_query.valid()) {
        if (m_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            applyStats(m_query.get());
        } catch (const std::exception& error) {
            qWarning() << "Peer statistics refresh failed:" << error.what();
        }
    }
    if (m_stopped || m_query.valid()) return;
    if (!m_refresh_requested) {
        m_result_timer->stop();
        return;
    }
    m_refresh_requested = false;
    auto result = m_worker.Submit([node = &m_node] {
        interfaces::Node::NodesStats nodes_stats;
        node->getNodesStats(nodes_stats);
        QList<CNodeCombinedStats> peers;
        peers.reserve(nodes_stats.size());
        for (auto& stats : nodes_stats) {
            peers.append(CNodeCombinedStats{std::move(std::get<0>(stats)), std::move(std::get<2>(stats)), std::get<1>(stats)});
        }
        return peers;
    });
    if (result) {
        m_query = std::move(*result);
        m_result_timer->start();
    }
}

void PeerTableModel::applyStats(QList<CNodeCombinedStats> new_peers_data)
{
    // Handle peer addition or removal as suggested in Qt Docs. See:
    // - https://doc.qt.io/qt-5/model-view-programming.html#inserting-and-removing-rows
    // - https://doc.qt.io/qt-5/model-view-programming.html#resizable-models
    // We take advantage of the fact that the std::vector returned
    // by interfaces::Node::getNodesStats is sorted by nodeid.
    for (int i = 0; i < m_peers_data.size();) {
        if (i < new_peers_data.size() && m_peers_data.at(i).nodeStats.nodeid == new_peers_data.at(i).nodeStats.nodeid) {
            ++i;
            continue;
        }
        // IDs are monotonic, so new peers can only be appended. Remove the
        // entire departed run in one model notification: disconnecting all
        // peers must not repeatedly shift the list and rebuild proxy mappings
        // once per peer on the GUI thread.
        int end = i + 1;
        while (end < m_peers_data.size() &&
               (i >= new_peers_data.size() || m_peers_data.at(end).nodeStats.nodeid != new_peers_data.at(i).nodeStats.nodeid)) {
            ++end;
        }
        beginRemoveRows(QModelIndex(), i, end - 1);
        m_peers_data.erase(m_peers_data.begin() + i, m_peers_data.begin() + end);
        endRemoveRows();
    }

    if (m_peers_data.size() < new_peers_data.size()) {
        // Some peers have been added to the end of the table.
        beginInsertRows(QModelIndex(), m_peers_data.size(), new_peers_data.size() - 1);
        m_peers_data.swap(new_peers_data);
        endInsertRows();
    } else {
        m_peers_data.swap(new_peers_data);
    }

    const auto top_left = index(0, 0);
    const auto bottom_right = index(rowCount() - 1, columnCount() - 1);
    if (!m_peers_data.empty()) Q_EMIT dataChanged(top_left, bottom_right);
}
