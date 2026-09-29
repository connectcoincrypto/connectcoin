// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <qt/clientmodel.h>

#include <qt/bantablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/peertablemodel.h>
#include <qt/peertablesortproxy.h>

#include <clientversion.h>
#include <common/args.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <net.h>
#include <netbase.h>
#include <util/time.h>
#include <validation.h>

#include <cstdint>
#include <array>
#include <deque>
#include <optional>
#include <utility>

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

static SteadyClock::time_point g_last_header_tip_update_notification{};
static SteadyClock::time_point g_last_block_tip_update_notification{};

// Disconnecting a btcsignals handler does not wait for callbacks that already
// entered. The gate outlives those callbacks and protects only enqueueing, not
// backend calls, GUI slots or an event-loop wait.
struct ClientModel::CoreSignalGate {
    Mutex mutex;
    ClientModel* target GUARDED_BY(mutex){nullptr};
    struct TipUpdate {
        SynchronizationState state;
        interfaces::BlockTip tip;
        double progress;
        SyncType type;
    };
    std::array<std::optional<TipUpdate>, 3> tips GUARDED_BY(mutex);
    bool tips_queued GUARDED_BY(mutex){false};
    bool state_refresh GUARDED_BY(mutex){false};
    bool ban_refresh GUARDED_BY(mutex){false};
    bool refresh_queued GUARDED_BY(mutex){false};
    struct ProgressUpdate {
        QString title;
        int progress;
    };
    std::deque<ProgressUpdate> progress GUARDED_BY(mutex);
    std::optional<ProgressUpdate> last_progress GUARDED_BY(mutex);
    bool progress_queued GUARDED_BY(mutex){false};

    // Called with the gate held. Verification/replay emits for every block,
    // not merely when the percentage changes. Keep only the latest pending
    // intermediate value, without dropping the dialog's start/end boundaries.
    static void PostProgress(const std::shared_ptr<CoreSignalGate>& gate) EXCLUSIVE_LOCKS_REQUIRED(gate->mutex)
    {
        AssertLockHeld(gate->mutex);
        auto* target = gate->target;
        const bool invoked = QMetaObject::invokeMethod(target, [target, gate] {
            std::array<std::optional<ProgressUpdate>, 64> updates;
            {
                LOCK(gate->mutex);
                if (gate->target != target) return;
                for (auto& update : updates) {
                    if (gate->progress.empty()) break;
                    update = std::move(gate->progress.front());
                    gate->progress.pop_front();
                }
            }
            const QPointer<ClientModel> guard{target};
            for (const auto& update : updates) {
                if (!guard || target->m_stopped) return;
                if (update) Q_EMIT target->showProgress(update->title, update->progress);
            }
            LOCK(gate->mutex);
            if (gate->target != target) return;
            if (gate->progress.empty()) gate->progress_queued = false;
            else PostProgress(gate);
        }, Qt::QueuedConnection);
        assert(invoked);
    }
};

ClientModel::ClientModel(interfaces::Node& node, OptionsModel *_optionsModel, QObject *parent) :
    QObject(parent),
    m_node(node),
    optionsModel(_optionsModel)
{
    cachedBestHeaderHeight = -1;
    cachedBestHeaderTime = -1;

    peerTableModel = new PeerTableModel(m_node, this);
    m_peer_table_sort_proxy = new PeerTableSortProxy(this);
    m_peer_table_sort_proxy->setSourceModel(peerTableModel);

    banTableModel = new BanTableModel(m_node, this);

    subscribeToCoreSignals();
    // A stalled chain/mempool snapshot must not prevent a mining command
    // from running. Each query type still has at most one outstanding job.
    m_worker.Start(2);
    m_poll_timer = new QTimer(this);
    m_poll_timer->setInterval(MODEL_UPDATE_DELAY);
    connect(m_poll_timer, &QTimer::timeout, this, [this] {
        pollNodeState();
        pollMiningStatus();
    });
    m_poll_timer->start();
    pollNodeState();
    pollMiningStatus();
}

void ClientModel::stop()
{
    stopWorkers();
}

void ClientModel::stopWorkers()
{
    if (m_stopped) return;
    m_stopped = true;
    m_poll_timer->stop();
    unsubscribeFromCoreSignals();
    // Stop every producer before entering a responsive nested event loop.
    // ThreadPool::Stop can execute queued work itself, so run that drain off
    // the GUI thread as well as waiting for currently running queries.
    peerTableModel->interrupt();
    banTableModel->interrupt();
    m_worker.Interrupt();
    peerTableModel->stop();
    banTableModel->stop();
    GUIUtil::WaitForBackendTask(std::async(std::launch::async, [worker = &m_worker] { worker->Stop(); }));
}

ClientModel::~ClientModel()
{
    // Normal application teardown drains while this QObject is intact.
    // Construction failures/tests still need a safe fallback, without an
    // event loop observing a partly destroyed model or its children.
    m_stopped = true;
    m_poll_timer->stop();
    unsubscribeFromCoreSignals();
    peerTableModel->interrupt();
    banTableModel->interrupt();
    m_worker.Stop();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    if (flags == CONNECTIONS_IN) return m_cached_state.connections_in;
    if (flags == CONNECTIONS_OUT) return m_cached_state.connections_out;
    if (flags == CONNECTIONS_ALL) return m_cached_state.connections_in + m_cached_state.connections_out;
    return 0;
}

int ClientModel::getHeaderTipHeight() const
{
    return cachedBestHeaderHeight;
}

int64_t ClientModel::getHeaderTipTime() const
{
    return cachedBestHeaderTime;
}


std::map<CNetAddr, LocalServiceInfo> ClientModel::getNetLocalAddresses() const
{
    return m_cached_state.local_addresses;
}


int ClientModel::getNumBlocks() const
{
    return m_cached_num_blocks;
}

uint256 ClientModel::getBestBlockHash()
{
    return WITH_LOCK(m_cached_tip_mutex, return m_cached_tip_blocks);
}

BlockSource ClientModel::getBlockSource() const
{
    if (m_cached_state.loading_blocks) return BlockSource::DISK;
    if (getNumConnections() > 0) return BlockSource::NETWORK;
    return BlockSource::NONE;
}

QString ClientModel::getStatusBarWarnings() const
{
    return m_cached_state.warnings;
}

OptionsModel *ClientModel::getOptionsModel()
{
    return optionsModel;
}

PeerTableModel *ClientModel::getPeerTableModel()
{
    return peerTableModel;
}

PeerTableSortProxy* ClientModel::peerTableSortProxy()
{
    return m_peer_table_sort_proxy;
}

BanTableModel *ClientModel::getBanTableModel()
{
    return banTableModel;
}

QString ClientModel::formatFullVersion() const
{
    return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatSubVersion() const
{
    return QString::fromStdString(strSubVersion);
}

bool ClientModel::isReleaseVersion() const
{
    return CLIENT_VERSION_IS_RELEASE;
}

QString ClientModel::formatClientStartupTime() const
{
    return QDateTime::currentDateTime().addSecs(-TicksSeconds(GetUptime())).toString();
}

QString ClientModel::dataDir() const
{
    return m_cached_state.data_dir;
}

QString ClientModel::blocksDir() const
{
    return m_cached_state.blocks_dir;
}

void ClientModel::TipChanged(SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress, SyncType synctype)
{
    if (synctype == SyncType::HEADER_SYNC) {
        // cache best headers time and height to reduce future cs_main locks
        cachedBestHeaderHeight = tip.block_height;
        cachedBestHeaderTime = tip.block_time;
    } else if (synctype == SyncType::BLOCK_SYNC) {
        m_cached_num_blocks = tip.block_height;
        WITH_LOCK(m_cached_tip_mutex, m_cached_tip_blocks = tip.block_hash;);
    }

    // Throttle GUI notifications about (a) blocks during initial sync, and (b) both blocks and headers during reindex.
    const bool throttle = (sync_state != SynchronizationState::POST_INIT && synctype == SyncType::BLOCK_SYNC) || sync_state == SynchronizationState::INIT_REINDEX;
    const auto now{throttle ? SteadyClock::now() : SteadyClock::time_point{}};
    auto& nLastUpdateNotification = synctype != SyncType::BLOCK_SYNC ? g_last_header_tip_update_notification : g_last_block_tip_update_notification;
    if (throttle && now < nLastUpdateNotification + MODEL_UPDATE_DELAY) {
        return;
    }

    Q_EMIT numBlocksChanged(tip.block_height, QDateTime::fromSecsSinceEpoch(tip.block_time), verification_progress, synctype, sync_state);
    nLastUpdateNotification = now;
}

void ClientModel::subscribeToCoreSignals()
{
    m_core_signal_gate = std::make_shared<CoreSignalGate>();
    const auto gate = m_core_signal_gate;
    WITH_LOCK(gate->mutex, gate->target = this);
    const auto enqueue_tip = [gate](SynchronizationState state, interfaces::BlockTip tip, double progress, SyncType type) {
        LOCK(gate->mutex);
        auto* target = gate->target;
        if (!target) return;
        gate->tips[static_cast<size_t>(type)] = CoreSignalGate::TipUpdate{state, tip, progress, type};
        if (gate->tips_queued) return;
        gate->tips_queued = true;
        QMetaObject::invokeMethod(target, [target, gate] {
            const QPointer<ClientModel> guard{target};
            std::array<std::optional<CoreSignalGate::TipUpdate>, 3> tips;
            {
                LOCK(gate->mutex);
                tips.swap(gate->tips);
                gate->tips_queued = false;
            }
            for (const auto& update : tips) {
                if (!guard || target->m_stopped) return;
                if (update) target->TipChanged(update->state, update->tip, update->progress, update->type);
            }
        }, Qt::QueuedConnection);
    };
    // Limiting backend snapshots alone does not limit the GUI event queue:
    // connection churn or a bulk ban update can still post thousands of
    // identical refresh callbacks. Merge these invalidations before posting.
    const auto enqueue_refresh = [gate](bool bans) {
        LOCK(gate->mutex);
        auto* target = gate->target;
        if (!target) return;
        (bans ? gate->ban_refresh : gate->state_refresh) = true;
        if (gate->refresh_queued) return;
        gate->refresh_queued = true;
        QMetaObject::invokeMethod(target, [target, gate] {
            const QPointer<ClientModel> guard{target};
            bool state_refresh, ban_refresh;
            {
                LOCK(gate->mutex);
                state_refresh = gate->state_refresh;
                ban_refresh = gate->ban_refresh;
                gate->state_refresh = gate->ban_refresh = gate->refresh_queued = false;
            }
            if (target->m_stopped) return;
            if (state_refresh) target->pollNodeState();
            if (!guard || target->m_stopped) return;
            if (ban_refresh) target->banTableModel->refresh();
        }, Qt::QueuedConnection);
    };
    m_event_handlers.emplace_back(m_node.handleShowProgress(
        [gate](const std::string& title, int progress, [[maybe_unused]] bool resume_possible) {
            CoreSignalGate::ProgressUpdate update{QString::fromStdString(title), progress};
            LOCK(gate->mutex);
            if (!gate->target) return;
            if (gate->last_progress && gate->last_progress->title == update.title && gate->last_progress->progress == progress) return;
            gate->last_progress = update;
            if (!gate->progress.empty()) {
                auto& last = gate->progress.back();
                const bool intermediate = progress > 0 && progress < 100 && last.progress > 0 && last.progress < 100;
                if (last.title == update.title && (intermediate || last.progress == progress)) {
                    last = std::move(update);
                    return;
                }
            }
            gate->progress.push_back(std::move(update));
            if (!std::exchange(gate->progress_queued, true)) CoreSignalGate::PostProgress(gate);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyNumConnectionsChanged(
        [enqueue_refresh](int) {
            enqueue_refresh(false);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyNetworkActiveChanged(
        [enqueue_refresh](bool) {
            enqueue_refresh(false);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyAlertChanged(
        [enqueue_refresh]() {
            enqueue_refresh(false);
        }));
    m_event_handlers.emplace_back(m_node.handleBannedListChanged(
        [enqueue_refresh]() {
            enqueue_refresh(true);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyBlockTip(
        [enqueue_tip](SynchronizationState sync_state, interfaces::BlockTip tip, double verification_progress) {
            enqueue_tip(sync_state, tip, verification_progress, SyncType::BLOCK_SYNC);
        }));
    m_event_handlers.emplace_back(m_node.handleNotifyHeaderTip(
        [enqueue_tip](SynchronizationState sync_state, interfaces::BlockTip tip, bool presync) {
            enqueue_tip(sync_state, tip, /*verification_progress=*/0.0, presync ? SyncType::HEADER_PRESYNC : SyncType::HEADER_SYNC);
        }));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    if (m_core_signal_gate) {
        LOCK(m_core_signal_gate->mutex);
        m_core_signal_gate->target = nullptr;
        m_core_signal_gate->tips = {};
        m_core_signal_gate->progress.clear();
        m_core_signal_gate->last_progress.reset();
        m_core_signal_gate->state_refresh = m_core_signal_gate->ban_refresh = false;
    }
    m_event_handlers.clear();
}

bool ClientModel::getProxyInfo(std::string& ip_port) const
{
    ip_port = m_cached_state.proxy;
    return m_cached_state.proxy_enabled;
}

void ClientModel::pollNodeState()
{
    if (m_stopped) return;
    if (m_state_query.valid()) {
        if (m_state_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            auto state = m_state_query.get();
            const bool connections_changed = !m_has_state || state.connections_in != m_cached_state.connections_in || state.connections_out != m_cached_state.connections_out;
            const bool network_changed = !m_has_state || state.network_active != m_cached_state.network_active;
            const bool warnings_changed = !m_has_state || state.warnings != m_cached_state.warnings;
            const bool metadata_changed = !m_has_state ||
                state.local_address_string != m_cached_state.local_address_string ||
                state.data_dir != m_cached_state.data_dir || state.blocks_dir != m_cached_state.blocks_dir ||
                state.proxy_enabled != m_cached_state.proxy_enabled || state.proxy != m_cached_state.proxy;
            // Core notifications can overtake this initial snapshot. Only
            // fill still-empty tip caches, never roll a newer tip backwards.
            int unknown_height{-1};
            cachedBestHeaderHeight.compare_exchange_strong(unknown_height, state.header_height);
            int64_t unknown_time{-1};
            cachedBestHeaderTime.compare_exchange_strong(unknown_time, state.header_time);
            unknown_height = -1;
            m_cached_num_blocks.compare_exchange_strong(unknown_height, state.blocks);
            WITH_LOCK(m_cached_tip_mutex, if (m_cached_tip_blocks.IsNull()) m_cached_tip_blocks = state.block_hash);
            m_cached_state = std::move(state);
            m_has_state = true;
            if (connections_changed) Q_EMIT numConnectionsChanged(getNumConnections());
            if (network_changed) Q_EMIT networkActiveChanged(m_cached_state.network_active);
            if (warnings_changed) Q_EMIT alertsChanged(m_cached_state.warnings);
            Q_EMIT mempoolSizeChanged(m_cached_state.mempool_size, m_cached_state.mempool_usage, m_cached_state.mempool_max_usage);
            Q_EMIT bytesChanged(m_cached_state.bytes_recv, m_cached_state.bytes_sent);
            if (metadata_changed) Q_EMIT nodeStateChanged();
        } catch (const std::exception& error) {
            qWarning() << "Node status refresh failed:" << error.what();
        }
    }
    // A signal receiver may have stopped the model or entered an event loop
    // that already scheduled the next query.
    if (m_stopped || m_state_query.valid()) return;
    const bool need_tip = cachedBestHeaderHeight == -1 || m_cached_num_blocks == -1 || getBestBlockHash().IsNull();
    m_state_query = requestNodeData([need_tip](interfaces::Node& node) {
        NodeState state;
        state.connections_in = node.getNodeCount(ConnectionDirection::In);
        state.connections_out = node.getNodeCount(ConnectionDirection::Out);
        state.network_active = node.getNetworkActive();
        state.initial_block_download = node.isInitialBlockDownload();
        state.loading_blocks = node.isLoadingBlocks();
        state.local_addresses = node.getNetLocalAddresses();
        // Address encoding (including onion checksums/base32) and joining are
        // snapshot preparation, not work for a GUI timer or hidden console.
        for (const auto& [address, info] : state.local_addresses) {
            if (!state.local_address_string.isEmpty()) state.local_address_string += QStringLiteral(", ");
            state.local_address_string += QString::fromStdString(address.ToStringAddr());
            if (!address.IsI2P()) state.local_address_string += QStringLiteral(":") + QString::number(info.nPort);
        }
        state.warnings = QString::fromStdString(node.getWarnings().translated);
        state.data_dir = GUIUtil::PathToQString(gArgs.GetDataDirNet());
        state.blocks_dir = GUIUtil::PathToQString(gArgs.GetBlocksDirPath());
        const auto ipv4 = node.getProxy(NET_IPV4);
        const auto ipv6 = node.getProxy(NET_IPV6);
        state.proxy_enabled = ipv4 && ipv6;
        if (state.proxy_enabled) state.proxy = ipv4->proxy.ToStringAddrPort();
        state.bytes_recv = node.getTotalBytesRecv();
        state.bytes_sent = node.getTotalBytesSent();
        state.mempool_size = node.getMempoolSize();
        state.mempool_usage = node.getMempoolDynamicUsage();
        state.mempool_max_usage = node.getMempoolMaxUsage();
        if (need_tip) {
            node.getHeaderTip(state.header_height, state.header_time);
            state.blocks = node.getNumBlocks();
            state.block_hash = node.getBestBlockHash();
        }
        return state;
    });
}

void ClientModel::pollMiningStatus()
{
    if (m_stopped) return;
    if (m_mining_command.valid() && m_mining_command.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        QString error;
        try {
            m_mining_status = m_mining_command.get();
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
        }
        Q_EMIT cpuMiningStatusChanged();
        Q_EMIT cpuMiningCommandFinished(error);
    }
    if (m_mining_query.valid()) {
        if (m_mining_query.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return;
        try {
            auto status = m_mining_query.get();
            if (m_mining_query_generation == m_mining_generation && !m_mining_command.valid()) {
                m_mining_status = std::move(status);
                Q_EMIT cpuMiningStatusChanged();
            }
        } catch (const std::exception& error) {
            qWarning() << "Mining status refresh failed:" << error.what();
        }
    }
    if (!m_stopped && !m_mining_command.valid() && !m_mining_query.valid()) {
        m_mining_query_generation = m_mining_generation;
        m_mining_query = requestNodeData([](interfaces::Node& node) { return node.getCpuMiningStatus(); });
    }
}

bool ClientModel::startCpuMining(const std::string& address, int threads)
{
    if (m_stopped || m_mining_command.valid()) return false;
    ++m_mining_generation;
    m_mining_command = requestNodeData([address, threads](interfaces::Node& node) {
        node.startCpuMining(address, threads);
        return node.getCpuMiningStatus();
    });
    Q_EMIT cpuMiningStatusChanged();
    return true;
}

bool ClientModel::stopCpuMining()
{
    if (m_stopped || m_mining_command.valid()) return false;
    ++m_mining_generation;
    m_mining_command = requestNodeData([](interfaces::Node& node) {
        node.stopCpuMining();
        return node.getCpuMiningStatus();
    });
    Q_EMIT cpuMiningStatusChanged();
    return true;
}
