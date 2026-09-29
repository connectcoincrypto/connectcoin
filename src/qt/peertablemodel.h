// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_PEERTABLEMODEL_H
#define CONNECTCOIN_QT_PEERTABLEMODEL_H

#include <net_processing.h>
#include <net.h>
#include <util/threadpool.h>

#include <QAbstractTableModel>
#include <QList>
#include <QModelIndex>
#include <QStringList>
#include <QVariant>

class PeerTablePriv;

namespace interfaces {
class Node;
}

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

struct CNodeCombinedStats {
    CNodeStats nodeStats;
    CNodeStateStats nodeStateStats;
    bool fNodeStateStatsAvailable;
};
Q_DECLARE_METATYPE(CNodeCombinedStats*)

/**
   Qt model providing information about connected peers, similar to the
   "getpeerinfo" RPC call. Used by the rpc console UI.
 */
class PeerTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit PeerTableModel(interfaces::Node& node, QObject* parent);
    ~PeerTableModel();
    void startAutoRefresh();
    void stopAutoRefresh();
    void stop();
    void interrupt();

    enum ColumnIndex {
        NetNodeId = 0,
        Age,
        Address,
        Direction,
        ConnectionType,
        Network,
        Ping,
        Sent,
        Received,
        Subversion
    };

    enum {
        StatsRole = Qt::UserRole,
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    /*@}*/

public Q_SLOTS:
    void refresh();

private:
    friend void CheckBatchedPeerUpdates(interfaces::Node& node);
    //! Internal peer data structure.
    QList<CNodeCombinedStats> m_peers_data{};
    interfaces::Node& m_node;
    ThreadPool m_worker{"qt-peers"};
    std::future<QList<CNodeCombinedStats>> m_query;
    QTimer* m_result_timer{nullptr};
    bool m_refresh_requested{false};
    bool m_stopped{false};
    bool m_worker_stopped{false};
    void pollRefresh();
    void applyStats(QList<CNodeCombinedStats> new_peers_data);
    const QStringList columns{
        /*: Title of Peers Table column which contains a
            unique number used to identify a connection. */
        tr("Peer"),
        /*: Title of Peers Table column which indicates the duration (length of time)
            since the peer connection started. */
        tr("Age"),
        /*: Title of Peers Table column which contains the
            IP/Onion/I2P address of the connected peer. */
        tr("Address"),
        /*: Title of Peers Table column which indicates the direction
            the peer connection was initiated from. */
        tr("Direction"),
        /*: Title of Peers Table column which describes the type of
            peer connection. The "type" describes why the connection exists. */
        tr("Type"),
        /*: Title of Peers Table column which states the network the peer
            connected through. */
        tr("Network"),
        /*: Title of Peers Table column which indicates the current latency
            of the connection with the peer. */
        tr("Ping"),
        /*: Title of Peers Table column which indicates the total amount of
            network information we have sent to the peer. */
        tr("Sent"),
        /*: Title of Peers Table column which indicates the total amount of
            network information we have received from the peer. */
        tr("Received"),
        /*: Title of Peers Table column which contains the peer's
            User Agent string. */
        tr("User Agent")};
    QTimer* timer{nullptr};
};

#endif // CONNECTCOIN_QT_PEERTABLEMODEL_H
