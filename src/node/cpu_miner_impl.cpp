// Copyright (c) 2026 The ConnectCoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Keep node-context dependencies out of the miner's lifecycle interface.
#include <node/cpu_miner.h>

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <interfaces/mining.h>
#include <interfaces/types.h>
#include <key_io.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <sync.h>
#include <util/chaintype.h>
#include <util/signalinterrupt.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace node {
CpuMiner::CpuMiner(NodeContext& node) : m_node{node}
{
    m_status.logical_cpus = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U,
                                                      static_cast<unsigned>(std::numeric_limits<int>::max())));
}

CpuMiner::~CpuMiner()
{
    Stop();
    if (m_thread.joinable()) m_thread.join();
}

void CpuMiner::Start(const std::string& address, int threads)
{
    const auto destination{DecodeDestination(address)};
    const CScript payout{GetScriptForDestination(destination)};
    const CTxOut output{0, payout};
    if (!IsValidDestination(destination) || output.GetType() != TxOutputType::P2PK || !output.GetP2PKPubKey()) {
        throw std::invalid_argument("Mining requires a type-1 P2PK address for this network");
    }
    if (!m_node.chainman || !m_node.mining || m_node.chainman->m_interrupt) {
        throw std::runtime_error("Node is not available for mining");
    }
    const auto chain{m_node.chainman->GetParams().GetChainType()};
    if (chain != ChainType::TESTNET4 && chain != ChainType::REGTEST) {
        throw std::invalid_argument("CPU mining is supported on testnet4 and regtest only");
    }
    std::lock_guard lock{m_mutex};
    if (threads < 1 || threads > m_status.max_threads) {
        throw std::invalid_argument("Threads must be between 1 and " + util::ToString(MAX_CPU_MINING_THREADS));
    }
    if (m_status.running) throw std::runtime_error("Miner is already running or stopping; stop it first");
    // The previous coordinator has finished all work before clearing running.
    if (m_thread.joinable()) m_thread.join();
    const int logical_cpus{m_status.logical_cpus};
    m_status = {};
    m_status.logical_cpus = logical_cpus;
    m_status.threads = threads;
    m_status.address = address;
    m_status.state = "starting";
    m_status.running = true;
    m_hashes = 0;
    m_stop = false;
    try {
        m_thread = std::thread{&CpuMiner::Run, this, payout, threads};
    } catch (...) {
        m_stop = true;
        m_status.running = false;
        m_status.state = "stopped";
        throw;
    }
}

void CpuMiner::Stop()
{
    std::lock_guard lock{m_mutex};
    m_stop = true;
    if (m_status.running) {
        m_status.stopping = true;
        m_status.state = "stopping";
    }
    m_wake.notify_all();
}

CpuMiningStatus CpuMiner::GetStatus() const
{
    std::lock_guard lock{m_mutex};
    auto status{m_status};
    status.hashes = m_hashes.load(std::memory_order_relaxed);
    return status;
}

void CpuMiner::Run(CScript payout, int threads)
{
    util::ThreadRename("cpu-miner");
    using Clock = std::chrono::steady_clock;
    auto sample_time{Clock::now()};
    uint64_t sample_hashes{0};
    try {
        auto& chainman{*m_node.chainman};
        auto& mining{*m_node.mining};
        const auto& consensus{chainman.GetConsensus()};
        while (!m_stop && !chainman.m_interrupt) {
            // Bootstrap testnet without peers is intentional. Never mine on a
            // known-behind tip while blocks are being downloaded/imported.
            bool behind{false};
            {
                LOCK(cs_main);
                const auto* tip{chainman.ActiveChain().Tip()};
                behind = !tip || (chainman.m_best_header && chainman.m_best_header->nChainWork > tip->nChainWork);
            }
            if (behind) {
                std::unique_lock lock{m_mutex};
                if (!m_stop) m_status.state = "waiting";
                m_status.hashes_per_second = 0;
                m_wake.wait_for(lock, std::chrono::milliseconds{100}, [&] { return m_stop.load(); });
                continue;
            }
            auto block_template{mining.createNewBlock({.coinbase_output_script = payout}, /*cooldown=*/false)};
            if (!block_template || m_stop) break;
            CBlock block{block_template->getBlock()};
            // Preserve height and the coinbase-input witness commitment. A
            // fresh extraNonce prevents repeated work after nonce exhaustion,
            // template refreshes, restarts, or independent miners sharing an address.
            CMutableTransaction coinbase{*block.vtx[0]};
            std::vector<unsigned char> extra_nonce(16);
            GetRandBytes(extra_nonce);
            coinbase.vin[0].scriptSig << extra_nonce;
            if (coinbase.vin[0].scriptSig.size() > 100) throw std::runtime_error("Coinbase extraNonce exceeds consensus limit");
            block.vtx[0] = MakeTransactionRef(std::move(coinbase));
            block.hashMerkleRoot = BlockMerkleRoot(block);
            uint256 key;
            int height;
            {
                LOCK(cs_main);
                const auto* prev{chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock)};
                if (!prev || prev != chainman.ActiveChain().Tip()) continue;
                key = GetRandomXKey(prev, consensus);
                height = prev->nHeight + 1;
            }

            std::atomic<bool> done{false};
            std::atomic<int> finished{0};
            std::mutex result_mutex;
            std::optional<uint32_t> solution;
            std::exception_ptr failure;
            const auto refresh_at{Clock::now() + std::chrono::seconds{5}};
            {
                std::lock_guard lock{m_mutex};
                if (!m_stop) m_status.state = "mining";
            }
            // One block/template, shared transactions, and one RandomX dataset
            // per key (the same context cache used by validation). Each worker
            // owns only a header and leases its own VM from that context.
            // libc++ 17, used by a supported CI configuration, has no jthread.
            // Always cancel and join already-created threads, including if
            // creating another thread or checking the tip throws.
            struct WorkerGroup {
                std::atomic<bool>& done;
                std::vector<std::thread> threads;
                void Join()
                {
                    done = true;
                    for (auto& thread : threads) if (thread.joinable()) thread.join();
                    threads.clear();
                }
                ~WorkerGroup() { Join(); }
            } workers{done, {}};
            workers.threads.reserve(threads);
            for (int worker{0}; worker < threads; ++worker) {
                // A solution or stop can arrive while a large worker group is
                // being created. Do not launch workers that have no work left.
                if (done || m_stop || chainman.m_interrupt) break;
                workers.threads.emplace_back([&, worker] {
                    util::ThreadRename("cpu-hash");
                    try {
                        CBlockHeader header{block};
                        // Use 64 bits for the stride so UINT32_MAX is tested
                        // without wraparound, duplicate work, or sanitizer errors.
                        for (uint64_t nonce{static_cast<uint64_t>(worker)}; nonce <= std::numeric_limits<uint32_t>::max(); nonce += static_cast<uint64_t>(threads)) {
                            if (done || m_stop || chainman.m_interrupt) break;
                            header.nNonce = static_cast<uint32_t>(nonce);
                            const bool valid{CheckProofOfWork(header, key, height, consensus)};
                            m_hashes.fetch_add(1, std::memory_order_relaxed);
                            if (valid) {
                                std::lock_guard lock{result_mutex};
                                if (!solution) solution = header.nNonce;
                                done = true;
                                break;
                            }
                        }
                    } catch (...) {
                        std::lock_guard lock{result_mutex};
                        failure = std::current_exception();
                        done = true;
                    }
                    ++finished;
                    m_wake.notify_all();
                });
            }
            while (!done && !m_stop && !chainman.m_interrupt && finished < threads && Clock::now() < refresh_at) {
                const auto tip{mining.getTip()};
                if (!tip || tip->hash != block.hashPrevBlock) break;
                std::unique_lock lock{m_mutex};
                m_wake.wait_for(lock, std::chrono::milliseconds{50}, [&] { return done || m_stop; });
            }
            workers.Join();
            if (failure) std::rethrow_exception(failure);
            // Include short successful rounds as well as long nonce searches.
            const auto now{Clock::now()};
            const double elapsed{std::chrono::duration<double>(now - sample_time).count()};
            if (elapsed >= 1) {
                const auto hashes{m_hashes.load(std::memory_order_relaxed)};
                std::lock_guard lock{m_mutex};
                m_status.hashes_per_second = static_cast<double>(hashes - sample_hashes) / elapsed;
                sample_hashes = hashes;
                sample_time = now;
            }
            if (solution && !m_stop && !chainman.m_interrupt) {
                const auto tip{mining.getTip()};
                if (!tip || tip->hash != block.hashPrevBlock) continue;
                block.nNonce = *solution;
                std::string reason, debug;
                const bool accepted{mining.submitBlock(block, reason, debug)};
                std::lock_guard lock{m_mutex};
                if (accepted) {
                    ++m_status.blocks;
                    m_status.error.clear();
                } else {
                    m_status.error = reason + ": " + debug;
                }
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard lock{m_mutex};
        m_status.error = e.what();
    }
    std::lock_guard lock{m_mutex};
    m_status.running = false;
    m_status.stopping = false;
    m_status.hashes_per_second = 0;
    m_status.state = m_status.error.empty() ? "stopped" : "error";
}
} // namespace node
