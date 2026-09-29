// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_QT_CLIENTMODEL_H
#define CONNECTCOIN_QT_CLIENTMODEL_H

#include <QObject>
#include <QDateTime>

#include <atomic>
#include <memory>
#include <sync.h>
#include <uint256.h>
#include <util/threadpool.h>

#include <net.h>
#include <node/cpu_miner_types.h>

class BanTableModel;
class CBlockIndex;
class OptionsModel;
class PeerTableModel;
class PeerTableSortProxy;
enum class SynchronizationState;
struct LocalServiceInfo;

namespace interfaces {
class Handler;
class Node;
struct BlockTip;
}

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

enum class BlockSource {
    NONE,
    DISK,
    NETWORK,
};

enum class SyncType {
    HEADER_PRESYNC,
    HEADER_SYNC,
    BLOCK_SYNC
};

enum NumConnections {
    CONNECTIONS_NONE = 0,
    CONNECTIONS_IN   = (1U << 0),
    CONNECTIONS_OUT  = (1U << 1),
    CONNECTIONS_ALL  = (CONNECTIONS_IN | CONNECTIONS_OUT),
};

/** Model for the ConnectCoin network client. */
class ClientModel : public QObject
{
    Q_OBJECT

public:
    explicit ClientModel(interfaces::Node& node, OptionsModel *optionsModel, QObject *parent = nullptr);
    ~ClientModel();

    void stop();
    void stopWorkers();

    interfaces::Node& node() const { return m_node; }
    //! Submit backend work with value-only captures. Results are consumed on
    //! the GUI thread; the worker is drained before node shutdown.
    template <typename Fn>
    auto requestNodeData(Fn&& query)
    {
        auto result = m_worker.Submit([node = &m_node, query = std::forward<Fn>(query)]() mutable {
            return query(*node);
        });
        if (!result) throw std::runtime_error("Node worker is unavailable");
        return std::move(*result);
    }
    OptionsModel *getOptionsModel();
    PeerTableModel *getPeerTableModel();
    PeerTableSortProxy* peerTableSortProxy();
    BanTableModel *getBanTableModel();

    //! Return number of connections, default is in- and outbound (total)
    int getNumConnections(unsigned int flags = CONNECTIONS_ALL) const;
    std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() const;
    QString getNetLocalAddressString() const { return m_cached_state.local_address_string; }
    int getNumBlocks() const;
    uint256 getBestBlockHash() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_tip_mutex);
    int getHeaderTipHeight() const;
    int64_t getHeaderTipTime() const;

    //! Returns the block source of the current importing/syncing state
    BlockSource getBlockSource() const;
    //! Return warnings to be displayed in status bar
    QString getStatusBarWarnings() const;

    QString formatFullVersion() const;
    QString formatSubVersion() const;
    bool isReleaseVersion() const;
    QString formatClientStartupTime() const;
    QString dataDir() const;
    QString blocksDir() const;

    bool getProxyInfo(std::string& ip_port) const;
    bool getNetworkActive() const { return m_cached_state.network_active; }
    bool isInitialBlockDownload() const { return m_cached_state.initial_block_download; }
    bool hasNodeState() const { return m_has_state; }
    quint64 getTotalBytesRecv() const { return m_cached_state.bytes_recv; }
    quint64 getTotalBytesSent() const { return m_cached_state.bytes_sent; }
    node::CpuMiningStatus getCpuMiningStatus() const { return m_mining_status; }
    bool cpuMiningCommandPending() const { return m_mining_command.valid(); }
    bool startCpuMining(const std::string& address, int threads);
    bool stopCpuMining();

    // caches for the best header: hash, number of blocks and block time
    mutable std::atomic<int> cachedBestHeaderHeight;
    mutable std::atomic<int64_t> cachedBestHeaderTime;
    mutable std::atomic<int> m_cached_num_blocks{-1};

    Mutex m_cached_tip_mutex;
    uint256 m_cached_tip_blocks GUARDED_BY(m_cached_tip_mutex){};

private:
    struct CoreSignalGate;
    interfaces::Node& m_node;
    std::shared_ptr<CoreSignalGate> m_core_signal_gate;
    std::vector<std::unique_ptr<interfaces::Handler>> m_event_handlers;
    OptionsModel *optionsModel;
    PeerTableModel* peerTableModel{nullptr};
    PeerTableSortProxy* m_peer_table_sort_proxy{nullptr};
    BanTableModel* banTableModel{nullptr};

    struct NodeState {
        int connections_in{0};
        int connections_out{0};
        bool network_active{false};
        bool initial_block_download{true};
        bool loading_blocks{false};
        std::map<CNetAddr, LocalServiceInfo> local_addresses;
        QString local_address_string;
        QString warnings;
        QString data_dir;
        QString blocks_dir;
        std::string proxy;
        bool proxy_enabled{false};
        quint64 bytes_recv{0};
        quint64 bytes_sent{0};
        long mempool_size{0};
        size_t mempool_usage{0};
        size_t mempool_max_usage{0};
        int header_height{-1};
        int64_t header_time{-1};
        int blocks{-1};
        uint256 block_hash;
    };
    ThreadPool m_worker{"qt-node"};
    QTimer* m_poll_timer{nullptr};
    std::future<NodeState> m_state_query;
    NodeState m_cached_state;
    std::future<node::CpuMiningStatus> m_mining_query;
    std::future<node::CpuMiningStatus> m_mining_command;
    node::CpuMiningStatus m_mining_status;
    uint64_t m_mining_generation{0};
    uint64_t m_mining_query_generation{0};
    bool m_stopped{false};
    bool m_has_state{false};

    void pollNodeState() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_tip_mutex);
    void pollMiningStatus();

    void TipChanged(SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress, SyncType synctype) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_tip_mutex);
    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

Q_SIGNALS:
    void numConnectionsChanged(int count);
    void numBlocksChanged(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType header, SynchronizationState sync_state);
    void mempoolSizeChanged(long count, size_t mempoolSizeInBytes, size_t mempoolMaxSizeInBytes);
    void networkActiveChanged(bool networkActive);
    void alertsChanged(const QString &warnings);
    void bytesChanged(quint64 totalBytesIn, quint64 totalBytesOut);
    //! Cached display metadata (paths, proxy or local addresses) first became
    //! available or changed. High-frequency statistics have separate signals.
    void nodeStateChanged();
    void cpuMiningStatusChanged();
    void cpuMiningCommandFinished(const QString& error);

    //! Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);

    // Show progress dialog e.g. for verifychain
    void showProgress(const QString &title, int nProgress);
};

#endif // CONNECTCOIN_QT_CLIENTMODEL_H
