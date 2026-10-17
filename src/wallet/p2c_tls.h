// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_TLS_H
#define CONNECTCOIN_WALLET_P2C_TLS_H

#include <netaddress.h>
#include <consensus/p2c.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/result.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wallet {
struct P2CTlsCaptureOptions {
    uint8_t signature_algorithms_mask{PayToDomainOutput::SIGNATURE_ALGORITHMS_ALL};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
    bool complete_handshake{false};
    /** Definitive local socket-family (true) or destination-route (false)
     * unavailability only. Refusals, timeouts and resource exhaustion do not
     * trigger this callback and must never disable an address family. */
    std::function<void(bool)> on_local_unavailable{};
    /** Admission/pacing hook before TCP connect. False aborts without starting
     * a connection. Cancellation/absolute deadline are checked again after it. */
    std::function<bool()> before_tcp_start{};
    /** Nonblocking final admission/accounting immediately before connect.
     * False aborts (e.g. a newly stale bounty) without a TCP attempt. Only a
     * true result may charge counters/pacing; socket-creation failures skip it. */
    std::function<bool()> on_tcp_started{};
};

/** Immutable owned wire bytes and their structurally checked parsed view.
 * This is not an authenticated proof: consensus verification remains required.
 * Copy/move retain the backing bytes; View spans live as long as an owner.
 */
class P2CCapturedProof {
    struct Storage;
    std::shared_ptr<const Storage> m_storage;
    explicit P2CCapturedProof(std::shared_ptr<const Storage> storage);
    friend util::Result<P2CCapturedProof> CaptureP2CTlsParsed(
        const CService&, const std::string&, const uint256&,
        const std::function<bool()>&, const P2CTlsCaptureOptions&);
public:
    const std::vector<unsigned char>& Bytes() const;
    const P2CTlsProofView& View() const;
    bool MatchesCaptureContext(const std::string& domain, const uint256& challenge) const;
};

/** Same capture policy, preserving the already parsed immutable proof. */
util::Result<P2CCapturedProof> CaptureP2CTlsParsed(
    const CService& endpoint, const std::string& domain, const uint256& challenge,
    const std::function<bool()>& cancelled, const P2CTlsCaptureOptions& options = {});
/** Resolve once, reject non-public addresses and never bypass a configured proxy. */
util::Result<std::vector<CService>> ResolveP2CDomain(const std::string& domain);

/** One TLS 1.3 handshake, no HTTP or private wallet keys. Returned bytes still
 * require consensus proof verification. Cancellation is polled during I/O.
 */
util::Result<std::vector<unsigned char>> CaptureP2CTls(
    const CService& endpoint, const std::string& domain, const uint256& challenge,
    const std::function<bool()>& cancelled, const P2CTlsCaptureOptions& options = {});

/** Public-endpoint capability probe. No wallet keys or HTTP request. Success
 * requires a complete RSA TLS handshake and a valid proof under the selected
 * immutable roots. DNS may be blocking; caller must run this off the UI thread
 * and ignore late results. No connection starts after cancellation/deadline.
 */
bool ProbeP2CRsa(const std::string& domain, uint32_t roots_version,
                 const std::function<bool()>& cancelled,
                 std::chrono::steady_clock::time_point deadline);

/** Stop probe access to process state before application teardown. Waits for
 * active network/crypto phases and their cleanup, never for blocking DNS.
 * Idempotent; no new probe starts after this call.
 */
void ShutdownP2CRsaProbes();
} // namespace wallet
#endif // CONNECTCOIN_WALLET_P2C_TLS_H
