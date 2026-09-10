// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c_worker.h>

#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <netaddress.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_domain_stats.h>
#include <wallet/p2c_tls.h>
#include <wallet/p2c_worker_threads.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wallet {
namespace {
using Clock = std::chrono::steady_clock;
using ScheduleClock = MockableSteadyClock;
constexpr size_t MAX_PENDING_CLAIMS{256};
constexpr size_t MAX_SAVED_STATE{64 * 1024 * 1024};
constexpr int RECENT_BOUNTY_BLOCKS{600};
constexpr auto SCHEDULE_REFRESH{std::chrono::seconds{5}};
constexpr auto DNS_REFRESH{std::chrono::seconds{60}};
constexpr auto STATE_KEY{"p2c_claim_worker_v1"};

std::optional<CTxDestination> RewardDestination(const std::string& address)
{
    if (address.empty()) return std::nullopt;
    auto destination{DecodeDestination(address)};
    if (!IsValidDestination(destination) || !CTxOut{0, GetScriptForDestination(destination)}.GetP2PKPubKey()) {
        throw std::invalid_argument("P2C claiming requires a type-1 P2PK reward address for this network");
    }
    return destination;
}
}

// Keep the wallet-facing lifecycle interface independent of its implementation.
class P2CClaimWorkerImpl final : public P2CClaimWorker {
public:
    explicit P2CClaimWorkerImpl(CWallet& wallet);
    ~P2CClaimWorkerImpl() override;
    util::Result<void> Configure(int rate, int concurrency, std::vector<std::string> domains, std::string reward_address) override;
    void Stop() override;
    void Shutdown() override;
    UniValue Status() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

struct P2CClaimWorkerImpl::Impl {
    CWallet& wallet;
    mutable std::mutex mutex;
    std::mutex control_mutex;
    std::condition_variable wake;
    // Some supported libc++ versions do not provide jthread/stop_token yet.
    // Reset cancellation only after joining the previous worker. The control
    // mutex serializes start/stop, and every connection joins before Run exits.
    std::thread thread;
    std::atomic<bool> stop_requested{false};
    bool closing{false};
    int rate{0};
    int concurrency{DEFAULT_P2C_CLAIM_CONCURRENCY};
    std::vector<std::string> domains;
    std::string reward_address;
    std::optional<CTxDestination> payout_destination;
    uint64_t attempts{0};
    uint64_t submitted{0};
    uint64_t domain_rounds{0};
    uint64_t schedule_refreshes{0};
    std::string state{"disabled"};
    std::string domain;
    std::string last_error;
    std::string capacity_error;
    std::string last_txid;
    Clock::time_point next_connection{};
    std::map<COutPoint, CTransactionRef> proposals;
    struct PriorityKey {
        P2CClaimPriority priority;
        COutPoint outpoint;
        bool operator<(const PriorityKey& other) const
        {
            if (priority != other.priority) return priority > other.priority;
            return outpoint < other.outpoint;
        }
    };
    struct DomainWork {
        struct MaskWork {
            std::set<PriorityKey> bounties;
            double connection_rate{5};
        };
        std::string name;
        std::map<PriorityKey, CTxOut> bounties;
        std::array<MaskWork, PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL + 1> masks;
        std::optional<PriorityKey> after;
        size_t endpoint_offset{0};
        std::vector<CService> endpoints;
        std::optional<std::pair<double, PriorityKey>> economic_key;
        ScheduleClock::time_point resolve_after{};
        bool resolving{false};
    };
    struct ClaimWork {
        P2CClaimProposal prepared;
        uint256 challenge;
        std::atomic<int64_t> validation_time;
        std::atomic<bool> unavailable{false};
        explicit ClaimWork(P2CClaimProposal value)
            : prepared(std::move(value)), challenge(P2CClaimChallenge(*prepared.tx, 0)), validation_time(prepared.validation_time) {}
    };
    struct Assignment {
        std::shared_ptr<DomainWork> group;
        std::shared_ptr<ClaimWork> claim;
        bool resolve{false};
        std::shared_ptr<P2CDomainStats> statistics{};
    };
    // No network I/O while holding this mutex. Proposals/proofs are serialized
    // here; immutable assigned work stays alive across schedule refreshes.
    std::mutex work_mutex;
    std::map<std::string, std::shared_ptr<DomainWork>> groups;
    // Exact masks can negotiate different server certificates and performance.
    // Share the last-100 history only within a (domain, mask), while domain
    // rotation and DNS stay shared. Preserve history through ineligibility.
    using DomainMask = std::pair<std::string, uint8_t>;
    std::map<DomainMask, std::shared_ptr<P2CDomainStats>> domain_statistics;
    std::map<COutPoint, std::shared_ptr<ClaimWork>> claim_cache;
    // Local search budget, independent of proposal-cache eviction, payout
    // changes and stop/start. Deliberately not saved to the wallet database.
    std::map<COutPoint, uint64_t> bounty_attempts;
    // Negative weighted score sorts best first; the exact bounty key breaks
    // floating-point ties. Store immutable snapshots, not a mutable comparator.
    std::set<std::pair<std::pair<double, PriorityKey>, std::string>> economic_order;
    std::optional<std::string> domain_after;
    bool prefer_reward{false};
    CCoinControl claim_control;
    // Persist a completed proof BEFORE submitting it; resuming never repeats
    // a search whose successful result was already durably saved.
    CTransactionRef ready;
    std::vector<unsigned char> ready_proof;
    std::optional<CTxDestination> ready_destination;
    struct CompletedClaim {
        CTransactionRef tx;
        std::vector<unsigned char> proof;
        std::optional<CTxDestination> destination;
    };
    // At most one proof per proposal. Only the coordinator submits;
    // capture threads append and persist under work_mutex.
    std::deque<CompletedClaim> completed;

    explicit Impl(CWallet& value) : wallet(value) {}

    void StopUnlocked()
    {
        if (thread.joinable()) {
            {
                std::lock_guard lock{mutex};
                stop_requested = true;
            }
            wake.notify_all();
            thread.join();
        }
        std::lock_guard lock{mutex};
        rate = 0;
    }

    void Message(std::string next_state, std::string error = {})
    {
        std::lock_guard lock{mutex};
        state = std::move(next_state);
        if (!error.empty()) last_error = std::move(error);
    }

    bool Pause(const std::atomic<bool>& stop, std::chrono::milliseconds delay)
    {
        std::unique_lock lock{mutex};
        wake.wait_for(lock, delay, [&] { return stop.load(); });
        return !stop.load();
    }

    void Save()
    {
        UniValue data{UniValue::VOBJ};
        UniValue pending{UniValue::VARR};
        for (const auto& [outpoint, tx] : proposals) pending.push_back(EncodeHexTx(*tx));
        data.pushKV("pending", pending);
        data.pushKV("ready", ready ? EncodeHexTx(*ready) : "");
        data.pushKV("proof", HexStr(ready_proof));
        data.pushKV("ready_address", ready_destination ? EncodeDestination(*ready_destination) : "");
        UniValue completed_data{UniValue::VARR};
        for (const auto& claim : completed) {
            UniValue item{UniValue::VOBJ};
            item.pushKV("tx", EncodeHexTx(*claim.tx));
            item.pushKV("proof", HexStr(claim.proof));
            item.pushKV("address", claim.destination ? EncodeDestination(*claim.destination) : "");
            completed_data.push_back(std::move(item));
        }
        data.pushKV("completed", std::move(completed_data));
        {
            std::lock_guard lock{mutex};
            data.pushKV("attempts", attempts);
            data.pushKV("submitted", submitted);
            data.pushKV("last_txid", last_txid);
        }
        const auto encoded{data.write()};
        if (encoded.size() > MAX_SAVED_STATE) throw std::runtime_error("P2C saved state exceeds limit");
        LOCK(wallet.cs_wallet);
        if (!wallet.GetDatabase().MakeBatch()->Write(std::string{STATE_KEY}, encoded)) {
            throw std::runtime_error("Could not persist P2C claim progress; worker stopped");
        }
    }

    void Load()
    {
        std::string encoded;
        {
            LOCK(wallet.cs_wallet);
            if (!wallet.GetDatabase().MakeBatch()->Read(std::string{STATE_KEY}, encoded)) return;
        }
        UniValue data;
        if (encoded.size() > MAX_SAVED_STATE || !data.read(encoded) || !data.isObject() ||
            !data["pending"].isArray() || data["pending"].size() > MAX_PENDING_CLAIMS) {
            throw std::runtime_error("Invalid saved P2C claim progress");
        }
        proposals.clear();
        for (const auto& item : data["pending"].getValues()) {
            CMutableTransaction tx;
            if (item.get_str().size() > 2048 || !DecodeHexTx(tx, item.get_str()) || tx.vin.size() != 1 || tx.vout.size() != 1 || tx.HasWitness()) {
                throw std::runtime_error("Invalid saved P2C proposal");
            }
            proposals.emplace(tx.vin[0].prevout, MakeTransactionRef(tx));
        }
        ready.reset();
        ready_proof.clear();
        ready_destination.reset();
        completed.clear();
        if (!data["ready"].get_str().empty()) {
            CMutableTransaction tx;
            if (data["ready"].get_str().size() > 2048 || !DecodeHexTx(tx, data["ready"].get_str()) ||
                tx.vin.size() != 1 || tx.vout.size() != 1 || tx.HasWitness() ||
                data["proof"].get_str().size() > MAX_P2C_PROOF_SIZE * 2 || !IsHex(data["proof"].get_str())) {
                throw std::runtime_error("Invalid saved P2C proof");
            }
            ready = MakeTransactionRef(tx);
            ready_proof = ParseHex(data["proof"].get_str());
            if (!data["ready_address"].isNull()) ready_destination = RewardDestination(data["ready_address"].get_str());
        }
        // Older wallets have only the single ready/proof slot.
        if (!data["completed"].isNull()) {
            if (!data["completed"].isArray() || data["completed"].size() + (ready ? 1 : 0) > MAX_PENDING_CLAIMS) {
                throw std::runtime_error("Invalid saved P2C completed claims");
            }
            std::set<COutPoint> seen;
            if (ready) seen.insert(ready->vin[0].prevout);
            for (const auto& item : data["completed"].getValues()) {
                CMutableTransaction tx;
                if (item["tx"].get_str().size() > 2048 || !DecodeHexTx(tx, item["tx"].get_str()) ||
                    tx.vin.size() != 1 || tx.vout.size() != 1 || tx.HasWitness() ||
                    item["proof"].get_str().size() > MAX_P2C_PROOF_SIZE * 2 || !IsHex(item["proof"].get_str()) ||
                    !seen.insert(tx.vin[0].prevout).second) {
                    throw std::runtime_error("Invalid saved P2C completed claim");
                }
                completed.push_back({MakeTransactionRef(tx), ParseHex(item["proof"].get_str()),
                    item["address"].isNull() ? std::nullopt : RewardDestination(item["address"].get_str())});
            }
        }
        std::lock_guard lock{mutex};
        attempts = data["attempts"].getInt<uint64_t>();
        submitted = data["submitted"].getInt<uint64_t>();
        last_txid = data["last_txid"].get_str();
    }

    bool Available(const COutPoint& outpoint)
    {
        if (wallet.chain().isSpentByMempool(outpoint)) return false;
        std::map<COutPoint, Coin> coins{{outpoint, Coin{}}};
        wallet.chain().findCoins(coins);
        return !coins.at(outpoint).IsSpent();
    }

    void SubmitReady(const std::atomic<bool>& stop)
    {
        while (!stop.load() && (ready || !completed.empty())) {
            if (!ready) {
                ready = std::move(completed.front().tx);
                ready_proof = std::move(completed.front().proof);
                ready_destination = std::move(completed.front().destination);
                completed.pop_front();
            }
            SubmitOne();
        }
    }

    void SubmitOne()
    {
        if (!ready) return;
        // Recognize a previously submitted/confirmed claim after a crash.
        if (WITH_LOCK(wallet.cs_wallet, return wallet.GetWalletTx(ready->GetHash()) != nullptr)) {
            claim_cache.erase(ready->vin[0].prevout);
            proposals.erase(ready->vin[0].prevout);
            ready.reset();
            ready_proof.clear();
            ready_destination.reset();
            Save();
            return;
        }
        auto validated{CompleteP2CClaim(wallet, *ready, ready_proof, ready_destination)};
        if (!validated) {
            if (Available(ready->vin[0].prevout)) {
                // A fee-policy change or reorg can be temporary. Do not throw
                // away expensive successful work, nor silently change its txid.
                throw std::runtime_error("Completed P2C proof retained; retry by enabling the worker again: " + util::ErrorString(validated).original);
            }
            Message("bounty spent", util::ErrorString(validated).original);
        } else {
            LOCK(wallet.cs_wallet);
            if (!wallet.GetBroadcastTransactions()) throw std::runtime_error("walletbroadcast is disabled; claim retained");
            wallet.CommitTransaction(*validated);
            std::lock_guard lock{mutex};
            ++submitted;
            last_txid = (*validated)->GetHash().GetHex();
            last_error.clear();
            state = wallet.chain().isInMempool((*validated)->GetHash()) ? "submitted" : "stored; check wallet history";
        }
        claim_cache.erase(ready->vin[0].prevout);
        proposals.erase(ready->vin[0].prevout);
        ready.reset();
        ready_proof.clear();
        ready_destination.reset();
        Save();
    }

    void UpdateEconomicOrder(DomainWork& group)
    {
        if (group.economic_key) economic_order.erase({*group.economic_key, group.name});
        group.economic_key.reset();
        // At most seven mask-specific leaders; never scan the whole domain
        // in the per-connection scheduling path.
        for (const auto& mask : group.masks) {
            if (mask.bounties.empty()) continue;
            const auto& best{*mask.bounties.begin()};
            const auto key{std::pair{-GetP2CDomainPriority(best.priority, mask.connection_rate), best}};
            if (!group.economic_key || key < *group.economic_key) group.economic_key = key;
        }
        if (group.economic_key) {
            economic_order.emplace(*group.economic_key, group.name);
        }
    }

    // Called only once per refresh, never for each connection. The node catalog
    // itself applies block deltas; only P2C entries are visited here.
    void Refresh(const std::atomic<bool>& stop)
    {
        std::map<COutPoint, CTxOut> snapshot;
        std::set<COutPoint> recent_outpoints;
        std::set<DomainMask> catalog_masks;
        if (!wallet.chain().scanP2CBounties([&](const COutPoint& outpoint, const CTxOut& output) {
            // Remember all recent entries before applying this wallet's
            // domain filter; changing the filter must not reset a budget.
            recent_outpoints.insert(outpoint);
            const auto bounty{output.GetPayToDomain()};
            if (domains.empty() || std::find(domains.begin(), domains.end(), bounty->domain) != domains.end()) {
                snapshot.emplace(outpoint, output);
                catalog_masks.emplace(bounty->domain, bounty->signature_algorithms_mask);
            }
            return true;
        }, [&] { return stop.load(); }, RECENT_BOUNTY_BLOCKS)) return;

        std::lock_guard work_lock{work_mutex};
        int wallet_height;
        int64_t validation_time{0};
        bool active{false};
        {
            LOCK(wallet.cs_wallet);
            claim_control.m_feerate = std::max({GetRequiredFeeRate(wallet), wallet.chain().mempoolMinFee(), wallet.chain().miningMinFee()});
            wallet_height = wallet.GetLastBlockHeight();
            wallet.chain().findBlock(wallet.GetLastBlockHash(), interfaces::FoundBlock().mtpTime(validation_time).inActiveChain(active));
        }
        const bool wallet_ready{active && wallet_height >= 0 && validation_time > 0};
        const auto fresh_fee{CalculateP2CClaimFee(*claim_control.m_feerate)};
        if (!fresh_fee) throw std::runtime_error(util::ErrorString(fresh_fee).original);
        // Aged-out idle work needs no repeated UTXO lookup. A live assignment
        // or completed proof keeps its entry until it has drained/submitted.
        std::erase_if(claim_cache, [&](const auto& entry) {
            return !snapshot.contains(entry.first) && entry.second.use_count() == 1 && !HasCompleted(entry.first);
        });
        // Confirmed-spent/aged-out bounties can be forgotten after live work
        // drains. Mempool spends, locks, fees and filters do not remove entries
        // from the recent catalog and therefore cannot reset their counters.
        std::erase_if(bounty_attempts, [&](const auto& entry) {
            return !recent_outpoints.contains(entry.first) && !claim_cache.contains(entry.first) && !HasCompleted(entry.first);
        });
        // One batched availability check for cached/in-flight challenges. A
        // competitor may waste up to one refresh interval, never redeem twice.
        std::map<COutPoint, Coin> coins;
        for (const auto& [outpoint, claim] : claim_cache) coins.try_emplace(outpoint);
        wallet.chain().findCoins(coins);
        // Retire previously cancelled work BEFORE rebuilding eligibility. Once
        // its users have drained, an unlocked/restored bounty can rejoin this
        // refresh rather than being excluded for an unnecessary extra interval.
        std::erase_if(claim_cache, [&](const auto& entry) {
            return entry.second->unavailable && entry.second.use_count() == 1 && !HasCompleted(entry.first);
        });
        // Availability is checked even for work that just aged out of the
        // discovery window. Age itself never cancels an in-flight handshake.
        for (auto& [outpoint, claim] : claim_cache) {
            const auto& coin{coins.at(outpoint)};
            if (!wallet_ready || coin.IsSpent() || coin.nHeight > static_cast<uint32_t>(wallet_height) ||
                (coin.IsCoinBase() && int64_t{wallet_height} + 1 - coin.nHeight < COINBASE_MATURITY) ||
                wallet.chain().isSpentByMempool(outpoint) ||
                WITH_LOCK(wallet.cs_wallet, return wallet.IsLockedCoin(outpoint))) claim->unavailable = true;
            if (!claim->unavailable) claim->validation_time = validation_time;
        }
        for (auto& [name, group] : groups) {
            group->bounties.clear();
            for (auto& mask : group->masks) mask.bounties.clear();
        }
        economic_order.clear();
        // Compute throughput once per (domain, mask) per refresh. An untried
        // mask retains the 5/s prior even if another mask cannot connect.
        std::map<DomainMask, double> connection_rates;
        for (const auto& [key, statistics] : domain_statistics) {
            connection_rates.emplace(key, statistics->ConnectionRate());
        }
        for (const auto& [outpoint, output] : snapshot) {
            if (stop.load()) return;
            const bool unavailable{!wallet_ready || wallet.chain().isSpentByMempool(outpoint) ||
                WITH_LOCK(wallet.cs_wallet, return wallet.IsLockedCoin(outpoint))};
            const auto cached{claim_cache.find(outpoint)};
            if (cached != claim_cache.end() && cached->second->unavailable) continue;
            if (unavailable) continue;
            const auto saved{proposals.find(outpoint)};
            const CAmount payout{saved != proposals.end() ? saved->second->vout[0].nValue :
                MoneyRange(output.nValue) && output.nValue > *fresh_fee ? output.nValue - *fresh_fee : 0};
            if (payout <= 0) continue;
            const auto bounty{*output.GetPayToDomain()};
            // GetUint64(3) is the MOST significant word of the little-endian
            // uint256. Even MAX_MONEY at the maximum smoothed rate (5005/s)
            // cannot reach 1000 connects/s when these 64 target bits are zero.
            if (bounty.connection_work_target.GetUint64(3) == 0) continue;
            if (AttemptLimitExceeded(outpoint, bounty.connection_work_target)) continue;
            const DomainMask domain_mask{bounty.domain, bounty.signature_algorithms_mask};
            const double connection_rate{connection_rates.try_emplace(domain_mask, 5.0).first->second};
            const auto priority{GetP2CClaimPriority(bounty.connection_work_target, payout)};
            // Filter every bounty BEFORE domain rotation, DNS, key reservation
            // or TLS. Guaranteed domain turns must not bypass this floor.
            if (!IsP2CClaimWorthAttempting(priority, connection_rate)) continue;
            auto& group{groups[bounty.domain]};
            if (!group) {
                group = std::make_shared<DomainWork>();
                group->name = bounty.domain;
            }
            auto& statistics{domain_statistics[domain_mask]};
            if (!statistics) statistics = std::make_shared<P2CDomainStats>();
            const PriorityKey key{priority, outpoint};
            group->bounties.emplace(key, output);
            auto& mask{group->masks[bounty.signature_algorithms_mask]};
            mask.bounties.insert(key);
            mask.connection_rate = connection_rate;
        }
        std::erase_if(groups, [](const auto& entry) { return entry.second->bounties.empty(); });
        // Preserve statistics while a domain/mask still has confirmed bounties,
        // even if all are temporarily locked/spent in the mempool. An old
        // in-flight assignment keeps its history alive until capture returns.
        std::erase_if(domain_statistics, [&](const auto& entry) {
            return !catalog_masks.contains(entry.first) && entry.second.use_count() == 1;
        });
        for (auto& [name, group] : groups) {
            group->economic_key.reset();
            UpdateEconomicOrder(*group);
        }
        if (groups.empty()) {
            std::lock_guard lock{mutex};
            domain.clear();
            state = snapshot.empty() ? "waiting for bounties" : "waiting for eligible bounties";
        }
        { std::lock_guard lock{mutex}; ++schedule_refreshes; }
        wake.notify_all();
    }

    bool HasCompleted(const COutPoint& outpoint) const
    {
        if (ready && ready->vin[0].prevout == outpoint) return true;
        return std::any_of(completed.begin(), completed.end(), [&](const auto& item) { return item.tx->vin[0].prevout == outpoint; });
    }

    bool AttemptLimitExceeded(const COutPoint& outpoint, const uint256& target) const
    {
        const auto found{bounty_attempts.find(outpoint)};
        return found != bounty_attempts.end() &&
            (found->second == std::numeric_limits<uint64_t>::max() || IsP2CClaimAttemptLimitExceeded(target, found->second));
    }

    std::optional<Assignment> Next(const std::atomic<bool>& stop)
    {
        std::lock_guard work_lock{work_mutex};
        // Do not preassign hundreds of future connections at a low rate: choose
        // against the latest schedule only when a connection slot is due.
        if (rate > 0 && Clock::now() < next_connection) return std::nullopt;
        const auto usable = [](const DomainWork& group) {
            return !group.bounties.empty() && !group.resolving &&
                (!group.endpoints.empty() || ScheduleClock::now() >= group.resolve_after);
        };
        while (!stop.load() && !groups.empty()) {
            std::shared_ptr<DomainWork> group;
            bool economic{false};
            if (prefer_reward) {
                for (const auto& [key, name] : economic_order) {
                    if (usable(*groups.at(name))) { group = groups.at(name); economic = true; break; }
                }
            } else {
                auto it{domain_after ? groups.upper_bound(*domain_after) : groups.begin()};
                for (size_t checked = 0; checked < groups.size(); ++checked) {
                    if (it == groups.end()) it = groups.begin();
                    if (usable(*it->second)) { group = it->second; break; }
                    ++it;
                }
            }
            if (!group) {
                if (economic_order.empty()) Message("waiting for eligible bounties");
                return std::nullopt;
            }
            auto candidate{group->after ? group->bounties.upper_bound(*group->after) : group->bounties.begin()};
            if (candidate == group->bounties.end()) candidate = group->bounties.begin();
            const auto key{candidate->first};
            const auto outpoint{key.outpoint};
            const auto mask{candidate->second.GetPayToDomain()->signature_algorithms_mask};
            const auto discard = [&] {
                group->masks[mask].bounties.erase(key);
                group->bounties.erase(candidate);
                UpdateEconomicOrder(*group);
            };
            if (AttemptLimitExceeded(outpoint, candidate->second.GetPayToDomain()->connection_work_target)) {
                discard();
                continue;
            }
            auto cached{claim_cache.find(outpoint)};
            if (cached != claim_cache.end() && cached->second->unavailable) {
                discard();
                continue;
            }
            if (cached == claim_cache.end()) {
                auto saved{proposals.find(outpoint)};
                if (saved == proposals.end() && proposals.size() >= MAX_PENDING_CLAIMS) {
                    // This is a bound on distinct persisted challenges, not
                    // threads, connections or domains. Never evict live work.
                    const auto old{std::find_if(proposals.begin(), proposals.end(), [&](const auto& entry) {
                        const auto task{claim_cache.find(entry.first)};
                        return !HasCompleted(entry.first) && (task == claim_cache.end() || task->second.use_count() == 1);
                    })};
                    if (old == proposals.end()) return std::nullopt;
                    claim_cache.erase(old->first);
                    proposals.erase(old);
                }
                auto prepared{saved == proposals.end() ? PrepareP2CClaim(wallet, outpoint, claim_control, MAX_P2C_PROOF_SIZE, payout_destination) :
                    ResumeP2CClaim(wallet, *saved->second, payout_destination)};
                if (!prepared) {
                    Message("bounty skipped", util::ErrorString(prepared).original);
                    discard();
                    continue;
                }
                // Fee policy may have changed since Refresh's quote. Check
                // the actual fixed payout before assigning any connection.
                if (!IsP2CClaimWorthAttempting(GetP2CClaimPriority(prepared->bounty.GetPayToDomain()->connection_work_target,
                                                                 prepared->tx->vout[0].nValue), group->masks[mask].connection_rate)) {
                    discard();
                    continue;
                }
                proposals.insert_or_assign(outpoint, prepared->tx);
                cached = claim_cache.emplace(outpoint, std::make_shared<ClaimWork>(std::move(*prepared))).first;
                Save(); // Fixed challenge is durable before ANY TLS connection.
            }
            if (ScheduleClock::now() >= group->resolve_after) {
                group->resolving = true;
                return Assignment{std::move(group), cached->second, true};
            }
            group->after = key;
            if (!economic) domain_after = group->name;
            prefer_reward = !economic; // A skipped domain never buys an extra.
            if (rate > 0) next_connection = Clock::now() + std::chrono::nanoseconds{(1'000'000'000LL + rate - 1) / rate};
            {
                std::lock_guard lock{mutex};
                ++domain_rounds; // Compatibility field: now counts assignments.
                domain = group->name;
                state = "searching";
            }
            const auto statistics{domain_statistics.at({group->name, mask})};
            return Assignment{std::move(group), cached->second, false, statistics};
        }
        return std::nullopt;
    }

    void Connect(const std::atomic<bool>& stop, const std::atomic<bool>& failed)
    {
        const auto cancelled = [&] { return stop.load() || failed.load(); };
        while (!cancelled()) {
            auto assignment{Next(stop)};
            if (!assignment) {
                auto until{Clock::now() + std::chrono::milliseconds{100}};
                {
                    std::lock_guard lock{work_mutex};
                    if (rate > 0 && next_connection > Clock::now()) until = std::min(until, next_connection);
                }
                std::unique_lock lock{mutex};
                wake.wait_until(lock, until, cancelled);
                continue;
            }
            const auto& group{assignment->group};
            if (assignment->resolve) {
                auto endpoints{ResolveP2CDomain(group->name)};
                {
                    std::lock_guard lock{work_mutex};
                    group->resolving = false;
                    group->resolve_after = ScheduleClock::now() + (endpoints ? DNS_REFRESH : std::chrono::seconds{2});
                    group->endpoints = endpoints ? std::move(*endpoints) : std::vector<CService>{};
                }
                wake.notify_all();
                if (!endpoints) {
                    Message("retrying domain resolution", util::ErrorString(endpoints).original);
                }
                continue; // DNS does not consume a fair/economic connection turn.
            }
            const auto& claim{assignment->claim};
            const auto claim_cancelled = [&] { return cancelled() || claim->unavailable.load(); };
            CService endpoint;
            {
                std::lock_guard lock{work_mutex};
                if (group->endpoints.empty() || claim_cancelled()) continue;
                const auto outpoint{claim->prepared.tx->vin[0].prevout};
                // Reserve the start under the shared lock: high concurrency
                // cannot overshoot the per-bounty budget. Already started
                // attempts may finish and submit a proof after this cutoff.
                if (AttemptLimitExceeded(outpoint, claim->prepared.bounty.GetPayToDomain()->connection_work_target)) continue;
                ++bounty_attempts[outpoint];
                { std::lock_guard status_lock{mutex}; ++attempts; }
                group->endpoint_offset %= group->endpoints.size();
                endpoint = group->endpoints[group->endpoint_offset];
                group->endpoint_offset = group->endpoint_offset + 1 == group->endpoints.size() ? 0 : group->endpoint_offset + 1;
            }
            if (claim_cancelled()) continue;
            // No chain/database lookup in the per-connection hot path.
            const auto started{Clock::now()};
            auto captured{CaptureP2CTls(endpoint, group->name, claim->challenge, claim_cancelled,
                {.signature_algorithms_mask = claim->prepared.bounty.GetPayToDomain()->signature_algorithms_mask})};
            const double seconds{std::chrono::duration<double>(Clock::now() - started).count()};
            if (captured || !claim_cancelled()) {
                std::lock_guard lock{work_mutex};
                // Full TLS capture is a success even if its work hash misses
                // the target. Local cancellation is not a server failure.
                assignment->statistics->Record(bool(captured), seconds);
            }
            if (!captured) {
                if (!claim_cancelled()) {
                    Message("retrying connections", util::ErrorString(captured).original);
                    Pause(stop, std::chrono::seconds{1});
                }
                continue;
            }
            const auto bounty{*claim->prepared.bounty.GetPayToDomain()};
            P2CTlsProofView view;
            std::string error;
            if (!ParseP2CTlsProof(*captured, bounty.domain, claim->challenge, view, error) ||
                !VerifyP2CCertificateProof(claim->prepared.bounty, view, claim->validation_time.load(), error)) {
                Message("certificate rejected", error);
                Pause(stop, std::chrono::seconds{1});
                continue;
            }
            if (!P2CMeetsWorkTarget(view.connection_work_hash, bounty.connection_work_target)) continue;
            std::lock_guard lock{work_mutex};
            if (!claim->unavailable.exchange(true)) {
                completed.push_back({claim->prepared.tx, std::move(*captured), payout_destination});
                Save(); // Only this bounty's duplicate attempts are cancelled.
                wake.notify_all();
            }
        }
    }

    void Run(const std::atomic<bool>& stop)
    {
        try {
            Load();
            groups.clear();
            claim_cache.clear();
            economic_order.clear();
            domain_after.reset();
            prefer_reward = false;
            SubmitReady(stop);
            // Completed proofs retain their original authorization above.
            // Unfinished challenges can be replaced when the user changes the
            // destination; never change the outputs of a successful proof.
            std::erase_if(proposals, [&](const auto& entry) {
                return !IsP2CClaimPayout(wallet, entry.second->vout[0], payout_destination);
            });
            Save();
            Refresh(stop);
            std::atomic<bool> failed{false};
            std::exception_ptr search_error;
            std::vector<std::thread> connections;
            P2CThreadPoolGrowth pool_growth;
            // Includes partial thread creation and coordinator exceptions.
            struct StopAndJoin {
                std::atomic<bool>& stop;
                std::condition_variable& wake;
                std::vector<std::thread>& connections;
                ~StopAndJoin()
                {
                    stop = true;
                    wake.notify_all();
                    for (auto& connection : connections) if (connection.joinable()) connection.join();
                }
            } stop_and_join{stop_requested, wake, connections};
            auto refresh_after{ScheduleClock::now() + SCHEDULE_REFRESH};
            while (!stop.load() && !failed) {
                // A huge configured concurrency with no eligible bounty must
                // remain idle, not allocate OS threads speculatively.
                const bool has_work{[&] { std::lock_guard lock{work_mutex}; return !economic_order.empty(); }()};
                if (has_work && connections.size() < static_cast<size_t>(concurrency)) {
                    const auto limited{pool_growth.Grow(connections, static_cast<size_t>(concurrency), [&] {
                        if (stop.load() || failed.load()) return false;
                        std::lock_guard lock{work_mutex};
                        return !economic_order.empty();
                    }, [&] {
                        return std::thread{[&] {
                            try {
                                Connect(stop, failed);
                            } catch (...) {
                                { std::lock_guard lock{work_mutex}; if (!search_error) search_error = std::current_exception(); }
                                failed = true;
                                wake.notify_all();
                            }
                        }};
                    })};
                    if (limited) {
                        std::lock_guard lock{mutex};
                        capacity_error = "P2C claiming limited to " + util::ToString(connections.size()) + " of " +
                            util::ToString(concurrency) + " requested connection threads: " + *limited +
                            ". Reconfigure claiming to retry.";
                    }
                }
                {
                    std::lock_guard lock{work_mutex};
                    SubmitReady(stop);
                }
                if (ScheduleClock::now() >= refresh_after) {
                    Refresh(stop);
                    refresh_after = ScheduleClock::now() + SCHEDULE_REFRESH;
                }
                Pause(stop, std::chrono::milliseconds{100});
            }
            stop_requested = true;
            wake.notify_all();
            for (auto& connection : connections) if (connection.joinable()) connection.join();
            if (search_error) std::rethrow_exception(search_error);
            Save();
            Message("disabled");
        } catch (const std::exception& e) {
            Message("stopped with error", e.what());
        }
        std::lock_guard lock{mutex};
        rate = 0;
    }
};

P2CClaimWorkerImpl::P2CClaimWorkerImpl(CWallet& wallet) : m_impl(std::make_unique<Impl>(wallet)) {}
P2CClaimWorkerImpl::~P2CClaimWorkerImpl() { Stop(); }

void P2CClaimWorkerImpl::Stop()
{
    std::lock_guard control{m_impl->control_mutex};
    m_impl->StopUnlocked();
}

void P2CClaimWorkerImpl::Shutdown()
{
    std::lock_guard control{m_impl->control_mutex};
    m_impl->closing = true;
    m_impl->StopUnlocked();
}

util::Result<void> P2CClaimWorkerImpl::Configure(int rate, int concurrency, std::vector<std::string> domains, std::string reward_address)
{
    std::lock_guard control{m_impl->control_mutex};
    if (m_impl->closing) return util::Error{Untranslated("Wallet is unloading")};
    if (rate < -1 || concurrency < 1 || domains.size() > 256) {
        return util::Error{Untranslated("Use rate -1, 0 or positive; positive concurrency; up to 256 domains")};
    }
    for (const auto& domain : domains) if (!IsCanonicalP2CDomain(domain)) return util::Error{Untranslated("Invalid P2C domain filter")};
    std::optional<CTxDestination> destination;
    try {
        destination = RewardDestination(reward_address);
    } catch (const std::invalid_argument& e) {
        return util::Error{Untranslated(e.what())};
    }
    if (rate != 0) {
        LOCK(m_impl->wallet.cs_wallet);
        if ((!destination && (m_impl->wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || m_impl->wallet.IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER))) ||
            !m_impl->wallet.GetBroadcastTransactions()) {
            return util::Error{Untranslated("Automatic claiming requires local wallet keys or an explicit reward address, and walletbroadcast enabled")};
        }
    }
    m_impl->StopUnlocked();
    {
        std::lock_guard lock{m_impl->mutex};
        m_impl->rate = rate;
        m_impl->concurrency = concurrency;
        m_impl->domains = std::move(domains);
        m_impl->reward_address = destination ? EncodeDestination(*destination) : "";
        m_impl->payout_destination = std::move(destination);
        m_impl->next_connection = {};
        m_impl->domain_rounds = 0;
        m_impl->schedule_refreshes = 0;
        m_impl->state = rate == 0 ? "disabled" : "starting";
        m_impl->last_error.clear();
        m_impl->capacity_error.clear();
    }
    if (rate != 0) {
        try {
            m_impl->stop_requested = false;
            m_impl->thread = std::thread{[this] { m_impl->Run(m_impl->stop_requested); }};
        } catch (const std::exception& e) {
            m_impl->Message("stopped with error", e.what());
            std::lock_guard lock{m_impl->mutex};
            m_impl->rate = 0;
            return util::Error{Untranslated(e.what())};
        }
    }
    return {};
}

UniValue P2CClaimWorkerImpl::Status() const
{
    std::lock_guard lock{m_impl->mutex};
    UniValue result{UniValue::VOBJ};
    result.pushKV("connections_per_second", m_impl->rate);
    result.pushKV("concurrency", m_impl->concurrency);
    result.pushKV("reward_address", m_impl->reward_address);
    result.pushKV("domain_rounds", m_impl->domain_rounds);
    result.pushKV("schedule_refreshes", m_impl->schedule_refreshes);
    result.pushKV("state", m_impl->state);
    result.pushKV("domain", m_impl->domain);
    result.pushKV("attempts", m_impl->attempts);
    result.pushKV("submitted", m_impl->submitted);
    // Successful claims may clear last_error, but must not hide reduced local
    // capacity. Keep connection/search errors visible alongside that warning.
    result.pushKV("last_error", m_impl->capacity_error.empty() ? m_impl->last_error :
        m_impl->capacity_error + (m_impl->last_error.empty() ? "" : " " + m_impl->last_error));
    result.pushKV("last_txid", m_impl->last_txid);
    return result;
}

std::unique_ptr<P2CClaimWorker> MakeP2CClaimWorker(CWallet& wallet)
{
    return std::make_unique<P2CClaimWorkerImpl>(wallet);
}
} // namespace wallet
