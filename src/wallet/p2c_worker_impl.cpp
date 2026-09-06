// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/p2c_worker.h>

#include <coins.h>
#include <consensus/p2c.h>
#include <consensus/p2c_x509.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <util/strencodings.h>
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
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace wallet {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t MAX_PENDING_CLAIMS{256};
constexpr size_t MAX_SAVED_STATE{32 * 1024 * 1024};
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
    std::jthread thread;
    bool closing{false};
    int rate{0};
    int concurrency{1};
    std::vector<std::string> domains;
    uint64_t attempts{0};
    uint64_t submitted{0};
    std::string state{"disabled"};
    std::string domain;
    std::string last_error;
    std::string last_txid;
    Clock::time_point next_connection{};
    std::map<COutPoint, CTransactionRef> proposals;
    std::optional<COutPoint> scan_after;
    // Persist a completed proof BEFORE submitting it; resuming never repeats
    // a search whose successful result was already durably saved.
    CTransactionRef ready;
    std::vector<unsigned char> ready_proof;

    explicit Impl(CWallet& value) : wallet(value) {}

    void StopUnlocked()
    {
        if (thread.joinable()) {
            thread.request_stop();
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

    bool Pause(std::stop_token stop, std::chrono::milliseconds delay)
    {
        std::unique_lock lock{mutex};
        wake.wait_for(lock, delay, [&] { return stop.stop_requested(); });
        return !stop.stop_requested();
    }

    void Save()
    {
        UniValue data{UniValue::VOBJ};
        UniValue pending{UniValue::VARR};
        for (const auto& [outpoint, tx] : proposals) pending.push_back(EncodeHexTx(*tx));
        data.pushKV("pending", pending);
        data.pushKV("ready", ready ? EncodeHexTx(*ready) : "");
        data.pushKV("proof", HexStr(ready_proof));
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

    void SubmitReady()
    {
        if (!ready) return;
        // Recognize a previously submitted/confirmed claim after a crash.
        {
            LOCK(wallet.cs_wallet);
            if (wallet.GetWalletTx(ready->GetHash())) {
                ready.reset();
                ready_proof.clear();
                return;
            }
        }
        auto completed{CompleteP2CClaim(wallet, *ready, ready_proof)};
        if (!completed) {
            if (Available(ready->vin[0].prevout)) {
                // A fee-policy change or reorg can be temporary. Do not throw
                // away expensive successful work, nor silently change its txid.
                throw std::runtime_error("Completed P2C proof retained; retry by enabling the worker again: " + util::ErrorString(completed).original);
            }
            Message("bounty spent", util::ErrorString(completed).original);
        } else {
            LOCK(wallet.cs_wallet);
            if (!wallet.GetBroadcastTransactions()) throw std::runtime_error("walletbroadcast is disabled; claim retained");
            wallet.CommitTransaction(*completed);
            std::lock_guard lock{mutex};
            ++submitted;
            last_txid = (*completed)->GetHash().GetHex();
            last_error.clear();
            state = wallet.chain().isInMempool((*completed)->GetHash()) ? "submitted" : "stored; check wallet history";
        }
        proposals.erase(ready->vin[0].prevout);
        ready.reset();
        ready_proof.clear();
        Save();
    }

    void Search(std::stop_token stop, const P2CClaimProposal& prepared)
    {
        const auto bounty{*prepared.bounty.GetPayToDomain()};
        {
            std::lock_guard lock{mutex};
            domain = bounty.domain;
            state = "resolving";
        }
        auto endpoints{ResolveP2CDomain(bounty.domain)};
        if (!endpoints) {
            Message("connection error", util::ErrorString(endpoints).original);
            Pause(stop, std::chrono::seconds{2});
            return;
        }
        const auto outpoint{prepared.tx->vin[0].prevout};
        const auto challenge{P2CClaimChallenge(*prepared.tx, 0)};
        std::atomic<bool> finished{false};
        std::atomic<unsigned> started{0};
        std::atomic<int> active{concurrency};
        const auto round_end{Clock::now() + std::chrono::seconds{30}};
        std::mutex proof_mutex;
        std::vector<unsigned char> proof;
        std::vector<std::jthread> connections;
        Message("searching");
        for (int i = 0; i < concurrency; ++i) {
            connections.emplace_back([&] {
                try {
                    auto cancelled = [&] {
                        return stop.stop_requested() || finished.load() || Clock::now() >= round_end;
                    };
                    while (!cancelled()) {
                        {
                            std::unique_lock lock{mutex};
                            while (rate > 0 && Clock::now() < next_connection && !cancelled()) {
                                wake.wait_until(lock, std::min(next_connection, Clock::now() + std::chrono::milliseconds{100}));
                            }
                            if (cancelled()) break;
                            if (rate > 0) next_connection = Clock::now() + std::chrono::nanoseconds{1'000'000'000LL / rate};
                        }
                        if (!Available(outpoint)) { finished = true; break; }
                        const auto attempt{started.fetch_add(1)};
                        if (attempt >= 32) break; // Fair rotation among independent bounties.
                        {
                            std::lock_guard lock{mutex};
                            ++attempts;
                        }
                        // Retry other resolved addresses even at concurrency=1
                        // (for example when only one IP family is reachable).
                        auto captured{CaptureP2CTls((*endpoints)[attempt % endpoints->size()], bounty.domain, challenge, cancelled)};
                        if (!captured) {
                            if (!cancelled()) {
                                Message("connection error", util::ErrorString(captured).original);
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
                        if (!finished.exchange(true)) proof = std::move(*captured);
                    }
                } catch (const std::exception& e) {
                    Message("connection error", e.what());
                    finished = true;
                }
                --active;
            });
        }
        // Monitor even during a slow network read, not just between attempts.
        while (!stop.stop_requested() && !finished && Clock::now() < round_end && active > 0) {
            if (!Available(outpoint)) { finished = true; Message("bounty spent"); break; }
            Pause(stop, std::chrono::milliseconds{200});
        }
        for (auto& connection : connections) connection.join();
        if (!proof.empty()) {
            ready = prepared.tx;
            ready_proof = std::move(proof);
            Save();
            if (!stop.stop_requested()) SubmitReady();
        }
    }

    void Run(std::stop_token stop)
    {
        try {
            Load();
            while (!stop.stop_requested()) {
                SubmitReady();
                Message("scanning confirmed bounties");
                // Select a bounded, rotating window over the entire snapshot.
                // Merely stopping at the first 256 UTXOs starves later bounties.
                std::map<COutPoint, CTxOut> next, wrapped;
                const bool scanned{wallet.chain().scanP2CBounties([&](const COutPoint& outpoint, const CTxOut& output) {
                    const auto bounty{output.GetPayToDomain()};
                    if (domains.empty() || std::find(domains.begin(), domains.end(), bounty->domain) != domains.end()) {
                        auto& window{!scan_after || *scan_after < outpoint ? next : wrapped};
                        window.emplace(outpoint, output);
                        if (window.size() > MAX_PENDING_CLAIMS) window.erase(std::prev(window.end()));
                    }
                    return true;
                }, [&] { return stop.stop_requested(); })};
                if (!scanned) { Pause(stop, std::chrono::seconds{5}); continue; }
                if (next.empty()) next = std::move(wrapped);
                scan_after = next.empty() ? std::nullopt : std::make_optional(next.rbegin()->first);
                for (auto it = proposals.begin(); it != proposals.end();) {
                    if (!next.contains(it->first) || !Available(it->first)) it = proposals.erase(it); else ++it;
                }
                std::vector<std::pair<COutPoint, CTxOut>> candidates{next.begin(), next.end()};
                // Highest reward first, but every candidate gets a bounded round.
                std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.second.nValue > b.second.nValue; });
                for (const auto& [outpoint, output] : candidates) {
                    if (stop.stop_requested()) break;
                    if (!Available(outpoint)) continue;
                    auto saved{proposals.find(outpoint)};
                    std::optional<P2CClaimProposal> prepared;
                    if (saved == proposals.end()) {
                        // A fresh node may have no fee-estimation history. Use
                        // the known economic relay/wallet and mempool floors,
                        // with the full proof budget and normal max-fee checks.
                        CCoinControl control;
                        {
                            LOCK(wallet.cs_wallet);
                            control.m_feerate = std::max({GetRequiredFeeRate(wallet), wallet.chain().mempoolMinFee(), wallet.chain().miningMinFee()});
                        }
                        auto result{PrepareP2CClaim(wallet, outpoint, control)};
                        if (!result) { Message("bounty skipped", util::ErrorString(result).original); continue; }
                        if (proposals.size() >= MAX_PENDING_CLAIMS) break;
                        proposals.emplace(outpoint, result->tx);
                        prepared = std::move(*result);
                        Save();
                    } else {
                        auto resumed{ResumeP2CClaim(wallet, *saved->second)};
                        if (!resumed) { Message("bounty skipped", util::ErrorString(resumed).original); continue; }
                        prepared = std::move(*resumed);
                    }
                    Search(stop, *prepared);
                    Save();
                }
                if (!stop.stop_requested()) { Message("waiting for bounties"); Pause(stop, std::chrono::seconds{60}); }
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
    if (rate < -1 || concurrency < 1 || concurrency > 64 || domains.size() > 256) {
        return util::Error{Untranslated("Use rate -1, 0 or positive; concurrency 1-64; up to 256 domains")};
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
        m_impl->state = rate == 0 ? "disabled" : "starting";
        m_impl->last_error.clear();
    }
    if (rate != 0) {
        try {
            m_impl->thread = std::jthread{[this](std::stop_token stop) { m_impl->Run(stop); }};
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
