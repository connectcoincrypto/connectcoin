// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef CONNECTCOIN_TEST_UTIL_CHAINPARAMS_H
#define CONNECTCOIN_TEST_UTIL_CHAINPARAMS_H

#include <kernel/chainparams.h>
#include <util/chaintype.h>

#include <memory>

class ArgsManager;

/** Mainnet rules with a retired development genesis, linked ONLY into tests.
 * Other networks use their unchanged production parameters. No daemon argument
 * or production factory can select this mainnet fixture.
 */
std::unique_ptr<const CChainParams> CreateChainParamsForTest(const ArgsManager& args, ChainType chain);

#endif // CONNECTCOIN_TEST_UTIL_CHAINPARAMS_H
