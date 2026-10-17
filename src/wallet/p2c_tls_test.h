// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_WALLET_P2C_TLS_TEST_H
#define CONNECTCOIN_WALLET_P2C_TLS_TEST_H

#include <cstddef>
#include <cstdint>

namespace wallet {
/** Test instrumentation for the calling worker only; no key material. */
struct P2CTlsReuseSnapshot {
    size_t setups, resets, discarded;
    uintptr_t input_buffer, output_buffer;
    bool clean;
};
P2CTlsReuseSnapshot P2CTlsReuseSnapshotForTest();
/** Force the next reset to discard storage after its normal secret cleanup. */
void FailNextP2CTlsResetForTest();
} // namespace wallet

#endif // CONNECTCOIN_WALLET_P2C_TLS_TEST_H
