// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c_worker.h>

#include <coins.h>
#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/p2c_claim.h>
#include <wallet/p2c_tls.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace wallet {
namespace {
using Clock = std::chrono::steady_clock;
using RoundClock = MockableSteadyClock;
constexpr size_t MAX_PENDING_CLAIMS{256};
constexpr size_t MAX_SAVED_STATE{64 * 1024 * 1024};
// Scheduling quantum, NOT a connection quota. Guaranteed fair turns alternate
// with economic-priority turns; a sole eligible domain has no cooldown.
constexpr auto DOMAIN_SCHEDULING_QUANTUM{std::chrono::seconds{30}};
constexpr auto STATE_KEY{"p2c_claim_worker_v1"};
}

// Keep the wallet-facing lifecycle interface independent of its implementation.
class P2CClaimWorkerImpl final : public P2CClaimWorker {
public:
    explicit P2CClaimWorkerImpl(CWallet& wallet);
    ~P2CClaimWorkerImpl() override;
    util::Result<void> Configure(int rate, int concurrency, std::vector<std::string> domains) override;
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
    int concurrency{4};
    std::vector<std::string> domains;
    uint64_t attempts{0};
    uint64_t submitted{0};
    uint64_t domain_rounds{0};
    std::string state{"disabled"};
    std::string domain;
    std::string last_error;
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
    struct DomainCursor {
        std::optional<PriorityKey> after;
        size_t endpoint_offset{0};
    };
    std::map<std::string, DomainCursor> scan_after;
    // Persist a completed proof BEFORE submitting it; resuming never repeats
    // a search whose successful result was already durably saved.
    CTransactionRef ready;
    std::vector<unsigned char> ready_proof;
    struct CompletedClaim {
        CTransactionRef tx;
        std::vector<unsigned char> proof;
    };
    // At most one proof per proposal. Only the search coordinator submits;
    // capture threads append and persist under the search's proof mutex.
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
        UniValue completed_data{UniValue::VARR};
        for (const auto& claim : completed) {
            UniValue item{UniValue::VOBJ};
            item.pushKV("tx", EncodeHexTx(*claim.tx));
            item.pushKV("proof", HexStr(claim.proof));
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
                completed.push_back({MakeTransactionRef(tx), ParseHex(item["proof"].get_str())});
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
            proposals.erase(ready->vin[0].prevout);
            ready.reset();
            ready_proof.clear();
            Save();
            return;
        }
        auto validated{CompleteP2CClaim(wallet, *ready, ready_proof)};
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
        proposals.erase(ready->vin[0].prevout);
        ready.reset();
        ready_proof.clear();
        Save();
    }

    std::optional<COutPoint> Search(const std::atomic<bool>& stop, const std::vector<P2CClaimProposal>& candidates,
                                  std::optional<RoundClock::time_point>& deadline)
    {
        if (stop.load() || (deadline && RoundClock::now() >= *deadline)) return std::nullopt;
        const auto search_domain{candidates.front().bounty.GetPayToDomain()->domain};
        {
            std::lock_guard lock{mutex};
            domain = search_domain;
            state = "resolving";
        }
        auto endpoints{ResolveP2CDomain(search_domain)};
        // Discovery, proposal preparation and the first DNS lookup must not
        // consume the entire search quantum before a connection can start.
        // Keep this deadline across later windows, including failed lookups,
        // so a domain cannot renew its turn indefinitely.
        if (!deadline) deadline = RoundClock::now() + DOMAIN_SCHEDULING_QUANTUM;
        const auto round_end{*deadline};
        if (!endpoints) {
            Message("retrying domain resolution", util::ErrorString(endpoints).original);
            Pause(stop, std::chrono::seconds{2});
            return std::nullopt;
        }
        auto& cursor{scan_after.at(search_domain)};
        const auto endpoint_offset{cursor.endpoint_offset % endpoints->size()};
        cursor.endpoint_offset = endpoint_offset == endpoints->size() - 1 ? 0 : endpoint_offset + 1;
        std::vector<uint256> challenges;
        challenges.reserve(candidates.size());
        for (const auto& prepared : candidates) challenges.push_back(P2CClaimChallenge(*prepared.tx, 0));
        std::vector<std::atomic<bool>> unavailable(candidates.size());
        for (auto& value : unavailable) value = false;
        std::atomic<bool> finished{false};
        std::atomic<int> active{0};
        // Bounded rotating indices, not an ever-growing attempt counter. There
        // is no attempt cap, including when just one connection is configured.
        size_t next_candidate{0}, next_endpoint{endpoint_offset};
        std::optional<size_t> last_candidate;
        std::mutex proof_mutex;
        std::exception_ptr search_error;
        std::vector<std::thread> connections;
        // If allocation or thread creation fails partway through launching,
        // cancel and join every existing thread before its captures disappear.
        struct StopAndJoin {
            std::atomic<bool>& finished;
            std::condition_variable& wake;
            std::vector<std::thread>& connections;
            ~StopAndJoin()
            {
                finished = true;
                wake.notify_all();
                for (auto& connection : connections) if (connection.joinable()) connection.join();
            }
        } stop_and_join{finished, wake, connections};
        Message("searching");
        for (int i = 0; i < concurrency; ++i) {
            if (stop.load() || finished || RoundClock::now() >= round_end) break;
            ++active;
            connections.emplace_back([&] {
                try {
                    auto cancelled = [&] {
                        return stop.load() || finished.load() || RoundClock::now() >= round_end;
                    };
                    while (!cancelled()) {
                        size_t index, endpoint_index;
                        {
                            std::unique_lock lock{mutex};
                            while (rate > 0 && Clock::now() < next_connection && !cancelled()) {
                                wake.wait_until(lock, std::min(next_connection, Clock::now() + std::chrono::milliseconds{100}));
                            }
                            if (cancelled()) break;
                            bool found{false};
                            for (size_t checked = 0; checked < candidates.size(); ++checked) {
                                index = next_candidate;
                                next_candidate = next_candidate + 1 == candidates.size() ? 0 : next_candidate + 1;
                                if (!unavailable[index]) { found = true; break; }
                            }
                            if (!found) break;
                            last_candidate = index;
                            endpoint_index = next_endpoint;
                            next_endpoint = next_endpoint + 1 == endpoints->size() ? 0 : next_endpoint + 1;
                            if (rate > 0) next_connection = Clock::now() + std::chrono::nanoseconds{(1'000'000'000LL + rate - 1) / rate};
                        }
                        const auto& prepared{candidates[index]};
                        // GetPayToDomain() returns an optional by value. Keep
                        // an owning copy across network I/O and verification.
                        const auto bounty{*prepared.bounty.GetPayToDomain()};
                        const auto& challenge{challenges[index]};
                        if (unavailable[index]) continue;
                        if (!Available(prepared.tx->vin[0].prevout)) { unavailable[index] = true; continue; }
                        const auto claim_cancelled = [&] { return cancelled() || unavailable[index].load(); };
                        {
                            std::lock_guard lock{mutex};
                            ++attempts;
                            state = "searching";
                        }
                        // Retry other resolved addresses even at concurrency=1
                        // (for example when only one IP family is reachable).
                        auto captured{CaptureP2CTls((*endpoints)[endpoint_index], search_domain, challenge, claim_cancelled)};
                        if (!captured) {
                            if (!claim_cancelled()) {
                                Message("retrying connections", util::ErrorString(captured).original);
                                Pause(stop, std::chrono::seconds{1});
                            }
                            continue;
                        }
                        P2CTlsProofView view;
                        std::string error;
                        if (!ParseP2CTlsProof(*captured, bounty.domain, challenge, view, error) ||
                            !VerifyP2CCertificateProof(prepared.bounty, view, prepared.validation_time, error)) {
                            Message("certificate rejected", error);
                            Pause(stop, std::chrono::seconds{1});
                            continue;
                        }
                        if (!P2CMeetsWorkTarget(view.connection_work_hash, bounty.connection_work_target)) continue;
                        std::lock_guard lock{proof_mutex};
                        if (!unavailable[index].exchange(true)) {
                            // Cancel only this bounty's duplicate attempts.
                            // Keep other bounties searching for the whole round.
                            completed.push_back({prepared.tx, std::move(*captured)});
                            Save(); // Persist every success before submission.
                            wake.notify_all();
                        }
                    }
                } catch (const std::exception& e) {
                    { std::lock_guard lock{proof_mutex}; if (!search_error) search_error = std::current_exception(); }
                    Message("stopped with error", e.what());
                    finished = true;
                }
                --active;
            });
        }
        // Monitor even during a slow network read, not just between attempts.
        while (!stop.load() && !finished && RoundClock::now() < round_end && active > 0) {
            { std::lock_guard lock{proof_mutex}; SubmitReady(stop); }
            // A competing claim only cancels work on its own output. Other
            // bounties on the same domain remain eligible for this round.
            std::map<COutPoint, Coin> coins;
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (!unavailable[i]) coins.try_emplace(candidates[i].tx->vin[0].prevout);
            }
            wallet.chain().findCoins(coins);
            bool any_available{false};
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& outpoint{candidates[i].tx->vin[0].prevout};
                if (!unavailable[i] && (coins.at(outpoint).IsSpent() || wallet.chain().isSpentByMempool(outpoint))) unavailable[i] = true;
                if (!unavailable[i]) any_available = true;
            }
            if (!any_available) { finished = true; Message("bounty spent"); break; }
            Pause(stop, std::chrono::milliseconds{200});
        }
        for (auto& connection : connections) connection.join();
        if (search_error) std::rethrow_exception(search_error);
        SubmitReady(stop);
        // Continue after the last assigned bounty, including when a deadline,
        // low connection rate ends a round early.
        if (last_candidate) return candidates[*last_candidate].tx->vin[0].prevout;
        return std::nullopt;
    }

    void Run(const std::atomic<bool>& stop)
    {
        try {
            Load();
            std::optional<std::string> domain_after;
            std::optional<std::string> active_domain;
            std::optional<RoundClock::time_point> round_end;
            bool prefer_reward{false};
            bool economic_round{false};
            bool round_started{false};
            bool empty_windows_wrapped{false};
            bool searched_since_wrap{false};
            while (!stop.load()) {
                if (active_domain && round_end && RoundClock::now() >= *round_end) active_domain.reset();
                SubmitReady(stop);
                Message("scanning confirmed bounties");
                CCoinControl control;
                {
                    LOCK(wallet.cs_wallet);
                    control.m_feerate = std::max({GetRequiredFeeRate(wallet), wallet.chain().mempoolMinFee(), wallet.chain().miningMinFee()});
                }
                const auto fresh_fee{CalculateP2CClaimFee(*control.m_feerate)};
                if (!fresh_fee) throw std::runtime_error(util::ErrorString(fresh_fee).original);
                // Select the next DOMAIN over the entire snapshot, regardless
                // of how many outputs it owns. Keep bounded output windows for
                // just two domains (next and wraparound), plus a cursor per
                // existing domain so large groups cannot starve their tail.
                struct DomainWindow {
                    std::string name;
                    std::map<PriorityKey, CTxOut> next, wrapped;
                } next_domain, wrapped_domain;
                std::string preferred_domain;
                std::optional<PriorityKey> preferred_key;
                // This is a memory bound, independent of attempts/concurrency.
                constexpr size_t window_size{MAX_PENDING_CLAIMS};
                std::set<std::string> seen_cursors;
                const bool scanned{wallet.chain().scanP2CBounties([&](const COutPoint& outpoint, const CTxOut& output) {
                    const auto bounty{output.GetPayToDomain()};
                    if (domains.empty() || std::find(domains.begin(), domains.end(), bounty->domain) != domains.end()) {
                        const auto cursor{scan_after.find(bounty->domain)};
                        if (cursor != scan_after.end()) seen_cursors.insert(bounty->domain);
                        if (active_domain && *active_domain != bounty->domain) return true;
                        const auto saved{proposals.find(outpoint)};
                        // Existing challenges retain their actual fixed fee.
                        // New claims all have the same one-input/one-P2PK size.
                        const CAmount payout{saved != proposals.end() ? saved->second->vout[0].nValue :
                            MoneyRange(output.nValue) && output.nValue > *fresh_fee ? output.nValue - *fresh_fee : 0};
                        const PriorityKey key{GetP2CClaimPriority(bounty->connection_work_target, payout), outpoint};
                        if (!active_domain && prefer_reward && payout > 0 &&
                            (!preferred_key || key < *preferred_key) &&
                            !wallet.chain().isSpentByMempool(outpoint) &&
                            !WITH_LOCK(wallet.cs_wallet, return wallet.IsLockedCoin(outpoint))) {
                            preferred_key = key;
                            preferred_domain = bounty->domain;
                        }
                        auto& group{active_domain || !domain_after || *domain_after < bounty->domain ? next_domain : wrapped_domain};
                        if (group.name.empty() || bounty->domain < group.name) group = DomainWindow{bounty->domain, {}, {}};
                        if (group.name != bounty->domain) return true;
                        auto& window{cursor == scan_after.end() || !cursor->second.after || *cursor->second.after < key ? group.next : group.wrapped};
                        window.emplace(key, output);
                        if (window.size() > window_size) window.erase(std::prev(window.end()));
                    }
                    return true;
                }, [&] { return stop.load(); })};
                if (!scanned) { Pause(stop, std::chrono::seconds{5}); continue; }
                std::erase_if(scan_after, [&](const auto& entry) { return !seen_cursors.contains(entry.first); });
                if (!active_domain && prefer_reward) {
                    prefer_reward = false;
                    if (!preferred_domain.empty()) {
                        // One preferential turn, then return to the fair cursor.
                        // Never move that cursor to the economic winner, or an
                        // unclaimable high reward could starve later domains.
                        active_domain = std::move(preferred_domain);
                        round_end.reset();
                        economic_round = true;
                        empty_windows_wrapped = false;
                        round_started = false;
                        continue; // Fill its bounded proposal window next.
                    }
                }
                const bool wrapped_cycle{!active_domain && next_domain.name.empty() && domain_after.has_value()};
                auto& group{next_domain.name.empty() ? wrapped_domain : next_domain};
                auto& candidates{group.next.empty() ? group.wrapped : group.next};
                if (candidates.empty()) {
                    // The active domain was exhausted. Select the next domain
                    // immediately; do not mistake this for an empty whole UTXO set.
                    if (active_domain) { active_domain.reset(); continue; }
                    {
                        std::lock_guard lock{mutex};
                        domain.clear();
                        state = "waiting for bounties";
                    }
                    domain_after.reset();
                    searched_since_wrap = false;
                    Pause(stop, std::chrono::seconds{5});
                    continue;
                }
                // Only idle after visiting EVERY domain without eligible work.
                // An unusable domain must not delay the next usable one.
                if (wrapped_cycle) {
                    if (!searched_since_wrap) {
                        Message("waiting for eligible bounties");
                        if (!Pause(stop, std::chrono::seconds{5})) break;
                    }
                    searched_since_wrap = false;
                }
                if (!active_domain) {
                    domain_after = group.name;
                    active_domain = group.name;
                    round_end.reset();
                    economic_round = false;
                    empty_windows_wrapped = false;
                    round_started = false;
                }
                scan_after[group.name].after = candidates.rbegin()->first;
                std::map<COutPoint, PriorityKey> candidate_keys;
                for (const auto& [key, output] : candidates) candidate_keys.emplace(key.outpoint, key);
                for (auto it = proposals.begin(); it != proposals.end();) {
                    if (!Available(it->first)) it = proposals.erase(it); else ++it;
                }
                std::vector<P2CClaimProposal> prepared_candidates;
                for (const auto& [key, output] : candidates) {
                    const auto& outpoint{key.outpoint};
                    if (stop.load()) break;
                    if (!Available(outpoint)) continue;
                    auto saved{proposals.find(outpoint)};
                    std::optional<P2CClaimProposal> prepared;
                    if (saved == proposals.end()) {
                        auto result{PrepareP2CClaim(wallet, outpoint, control)};
                        if (!result) { Message("bounty skipped", util::ErrorString(result).original); continue; }
                        if (proposals.size() >= MAX_PENDING_CLAIMS) {
                            // No search is in flight here. Evict an old proposal
                            // outside this round, never a completed proof.
                            const auto old{std::find_if(proposals.begin(), proposals.end(), [&](const auto& entry) { return !candidate_keys.contains(entry.first); })};
                            if (old == proposals.end()) break;
                            proposals.erase(old);
                        }
                        proposals.emplace(outpoint, result->tx);
                        prepared = std::move(*result);
                    } else {
                        auto resumed{ResumeP2CClaim(wallet, *saved->second)};
                        if (!resumed) { Message("bounty skipped", util::ErrorString(resumed).original); continue; }
                        prepared = std::move(*resumed);
                    }
                    prepared_candidates.push_back(std::move(*prepared));
                }
                // Preparation rechecks current fee floors. Order by the actual
                // payout even if fee policy changed during the snapshot scan.
                std::stable_sort(prepared_candidates.begin(), prepared_candidates.end(), [](const auto& a, const auto& b) {
                    return GetP2CClaimPriority(a.bounty.GetPayToDomain()->connection_work_target, a.tx->vout[0].nValue) >
                           GetP2CClaimPriority(b.bounty.GetPayToDomain()->connection_work_target, b.tx->vout[0].nValue);
                });
                Save(); // Persist all fixed challenges before starting HTTPS.
                if (!stop.load() && !prepared_candidates.empty()) {
                    if (!round_started) {
                        { std::lock_guard lock{mutex}; ++domain_rounds; }
                        // Skipping an unusable fair domain must NOT grant an
                        // extra economic turn. Otherwise many locked/dust-only
                        // domains could amplify the winner's allocation.
                        if (!economic_round) prefer_reward = true;
                        round_started = true;
                    }
                    searched_since_wrap = true;
                    empty_windows_wrapped = false;
                    if (const auto last{Search(stop, prepared_candidates, round_end)}) scan_after.at(group.name).after = candidate_keys.at(*last);
                    Save();
                } else if (group.next.empty() && std::exchange(empty_windows_wrapped, true)) {
                    // A full output cycle contained no eligible work. Continue
                    // to the next domain instead of repeatedly scanning it.
                    active_domain.reset();
                }
            }
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

util::Result<void> P2CClaimWorkerImpl::Configure(int rate, int concurrency, std::vector<std::string> domains)
{
    std::lock_guard control{m_impl->control_mutex};
    if (m_impl->closing) return util::Error{Untranslated("Wallet is unloading")};
    if (rate < -1 || concurrency < 1 || domains.size() > 256) {
        return util::Error{Untranslated("Use rate -1, 0 or positive; positive concurrency; up to 256 domains")};
    }
    for (const auto& domain : domains) if (!IsCanonicalP2CDomain(domain)) return util::Error{Untranslated("Invalid P2C domain filter")};
    if (rate != 0) {
        LOCK(m_impl->wallet.cs_wallet);
        if (m_impl->wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || m_impl->wallet.IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER) ||
            !m_impl->wallet.GetBroadcastTransactions()) {
            return util::Error{Untranslated("Automatic claiming requires local wallet keys and walletbroadcast enabled")};
        }
    }
    m_impl->StopUnlocked();
    {
        std::lock_guard lock{m_impl->mutex};
        m_impl->rate = rate;
        m_impl->concurrency = concurrency;
        m_impl->domains = std::move(domains);
        m_impl->next_connection = {};
        m_impl->domain_rounds = 0;
        m_impl->state = rate == 0 ? "disabled" : "starting";
        m_impl->last_error.clear();
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
    result.pushKV("domain_rounds", m_impl->domain_rounds);
    result.pushKV("state", m_impl->state);
    result.pushKV("domain", m_impl->domain);
    result.pushKV("attempts", m_impl->attempts);
    result.pushKV("submitted", m_impl->submitted);
    result.pushKV("last_error", m_impl->last_error);
    result.pushKV("last_txid", m_impl->last_txid);
    return result;
}

std::unique_ptr<P2CClaimWorker> MakeP2CClaimWorker(CWallet& wallet)
{
    return std::make_unique<P2CClaimWorkerImpl>(wallet);
}
} // namespace wallet
