// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_TLS_H
#define CONNECTCOIN_WALLET_P2C_TLS_H

#include <netaddress.h>
#include <uint256.h>
#include <util/result.h>

#include <functional>
#include <string>
#include <vector>

namespace wallet {
/** Resolve once, reject non-public addresses and never bypass a configured proxy. */
util::Result<std::vector<CService>> ResolveP2CDomain(const std::string& domain);

/** One TLS 1.3 handshake, no HTTP or private wallet keys. Returned bytes still
 * require consensus proof verification. Cancellation is polled during I/O.
 */
util::Result<std::vector<unsigned char>> CaptureP2CTls(
    const CService& endpoint, const std::string& domain, const uint256& challenge,
    const std::function<bool()>& cancelled);
} // namespace wallet
#endif // CONNECTCOIN_WALLET_P2C_TLS_H
