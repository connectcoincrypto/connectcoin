// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see COPYING.
// Offline launch-genesis search. Uses Core's actual typed transaction/header
// serialization and the bundled RandomX v2 implementation. No node, wallet,
// network, datadir, or private key is involved.
// Default: search with six threads and a 300-second best-effort deadline,
// measured from before dataset initialization and checked between hashes.
// Dataset initialization and an in-flight hash cannot be cancelled; verification
// follows the search. --verify NONCE checks LIGHT without starting a search.
// After a Release build, compile in an x64 MSVC developer prompt at repo root:
// cl /std:c++20 /EHsc /O2 /MD /DWIN32 /DSECP256K1_STATIC /DNOMINMAX
//    /DWIN32_LEAN_AND_MEAN /Isrc /Ibuild/src /Isrc/randomx/src
//    contrib/devtools/mine-mainnet-genesis.cpp /Fe:mine-mainnet-genesis.exe
//    /link build/lib/Release/connectcoin_common.lib
//    build/lib/Release/connectcoin_consensus.lib
//    build/lib/Release/connectcoin_util.lib build/lib/Release/connectcoin_crypto.lib
//    build/lib/Release/connectcoin_clientversion.lib
//    build/src/randomx/Release/randomx.lib
//    build/src/secp256k1/lib/Release/libsecp256k1.lib
//    bcrypt.lib ws2_32.lib iphlpapi.lib shlwapi.lib advapi32.lib
// Join those comment lines into one command. Adapt build/library paths for
// another compiler; the search and verification logic is portable C++20.

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <crypto/hex_base.h>
#include <crypto/randomx_util.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <span.h>
#include <streams.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {
constexpr std::string_view HEADLINE{"Cloudflare 01/Oct/2026 Support for modern cryptographic algorithms in Workers"};
constexpr std::string_view PUBLIC_KEYS[]{
    "29a6b41260ed25e3019d18236192afecec9ea0b7312837bacf81a3c40d7b2034",
    "2c83a568529b55cc93e2d20754f079f6072d77ac88266d9b054cb3e0e92a7845",
};
constexpr uint32_t GENESIS_TIME{1790872995}; // 2026-10-01 16:43:15 UTC
constexpr uint32_t GENESIS_BITS{0x1e333300};
constexpr uint256 BOOTSTRAP_KEY{"d91b262aecaac2c4868b2cbe1563538f107c33fbee8c5d373bdaa8e551567fe5"};
constexpr unsigned THREADS{6};

CBlock MakeGenesis()
{
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig = CScript{} << 486604799 << CScriptNum{4}
        << std::vector<unsigned char>{HEADLINE.begin(), HEADLINE.end()};
    if (coinbase.vin[0].scriptSig.size() > 100) throw std::runtime_error("Coinbase headline is too long");
    for (const auto key_hex : PUBLIC_KEYS) {
        const auto key_bytes{ParseHex(key_hex)};
        coinbase.vout.emplace_back(5'000'000 * COIN, XOnlyPubKey{key_bytes});
    }
    CBlock block;
    block.nVersion = 1;
    block.nTime = GENESIS_TIME;
    block.nBits = GENESIS_BITS;
    block.nNonce = 0;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

DataStream HeaderBytes(const CBlockHeader& header)
{
    DataStream stream;
    stream << header;
    if (stream.size() != 80) throw std::runtime_error("Header must contain exactly 80 bytes");
    return stream;
}

uint256 HashHeader(const RandomXContext& context, const CBlockHeader& header)
{
    const auto bytes{HeaderBytes(header)};
    const auto hash{context.Calculate(MakeByteSpan(bytes))};
    return uint256{MakeUCharSpan(hash)};
}
} // namespace

int main(int argc, char** argv)
try {
    const bool verify{argc == 3 && std::string_view{argv[1]} == "--verify"};
    if (argc != 1 && !verify) throw std::runtime_error("Usage: mine-mainnet-genesis [--verify NONCE]");
    CBlock block{MakeGenesis()};
    arith_uint256 target;
    target.SetCompact(GENESIS_BITS);
    std::atomic<uint64_t> attempts{0};
    const auto started{std::chrono::steady_clock::now()};
    uint256 fast_hash;
    if (verify) {
        uint32_t nonce{0};
        const std::string_view argument{argv[2]};
        const auto [end, error]{std::from_chars(argument.data(), argument.data() + argument.size(), nonce)};
        if (error != std::errc{} || end != argument.data() + argument.size()) {
            throw std::runtime_error("Invalid nonce");
        }
        block.nNonce = nonce;
    } else {
        std::cerr << "Initializing shared FAST RandomX v2 dataset; timestamp=" << GENESIS_TIME
                  << ", bits=0x1e333300, threads=" << THREADS << '\n';
        RandomXOptions options;
        options.dataset_init_threads = THREADS;
        const RandomXContext context{RandomXAlgorithm::V2, MakeByteSpan(BOOTSTRAP_KEY), RandomXMemoryMode::FAST, options};
        std::atomic<bool> done{false};
        std::atomic<unsigned> remaining{THREADS};
        std::atomic<uint64_t> solution{uint64_t{1} << 32};
        std::mutex error_mutex;
        std::exception_ptr error;
        const auto deadline{started + std::chrono::seconds{300}};
        // Join all launched workers even if creating a later thread throws.
        struct WorkerGroup {
            std::atomic<bool>& done;
            std::vector<std::thread> threads;
            void Join()
            {
                done.store(true);
                for (auto& worker : threads) if (worker.joinable()) worker.join();
                threads.clear();
            }
            ~WorkerGroup() { Join(); }
        } workers{done, {}};
        workers.threads.reserve(THREADS);
        for (unsigned lane{0}; lane < THREADS; ++lane) {
            workers.threads.emplace_back([&, lane] {
                try {
                    CBlockHeader header{block};
                    for (uint64_t nonce{lane}; nonce <= std::numeric_limits<uint32_t>::max(); nonce += THREADS) {
                        if (done.load() || std::chrono::steady_clock::now() >= deadline) break;
                        header.nNonce = static_cast<uint32_t>(nonce);
                        const auto hash{HashHeader(context, header)};
                        ++attempts;
                        if (UintToArith256(hash) <= target) {
                            uint64_t empty{uint64_t{1} << 32};
                            solution.compare_exchange_strong(empty, nonce);
                            done.store(true);
                            break;
                        }
                    }
                } catch (...) {
                    std::lock_guard lock{error_mutex};
                    if (!error) error = std::current_exception();
                    done.store(true);
                }
                --remaining;
            });
        }
        unsigned elapsed{0};
        while (remaining.load() != 0) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
            if (++elapsed % 5 == 0) std::cerr << "hashes=" << attempts.load() << '\n';
        }
        workers.Join();
        if (error) std::rethrow_exception(error);
        if (solution.load() > std::numeric_limits<uint32_t>::max()) {
            std::cerr << "No nonce found before the search deadline; hashes=" << attempts.load() << '\n';
            return 2;
        }
        block.nNonce = static_cast<uint32_t>(solution.load());
        fast_hash = HashHeader(context, block);
        if (UintToArith256(fast_hash) > target) throw std::runtime_error("FAST verification failed");
        std::cerr << "Found nonce=" << block.nNonce << "; independently verifying LIGHT interpreter\n";
    }

    RandomXOptions light_options;
    light_options.try_large_pages = false;
    light_options.use_jit = false;
    const RandomXContext light{RandomXAlgorithm::V2, MakeByteSpan(BOOTSTRAP_KEY), RandomXMemoryMode::LIGHT, light_options};
    const uint256 pow_hash{HashHeader(light, block)};
    if (UintToArith256(pow_hash) > target) throw std::runtime_error("LIGHT proof of work is invalid");
    if (!verify && pow_hash != fast_hash) throw std::runtime_error("FAST/LIGHT hashes differ");
    DataStream tx_bytes;
    tx_bytes << TX_NO_WITNESS(*block.vtx.front());
    DataStream block_bytes;
    block_bytes << TX_NO_WITNESS(block);
    const double elapsed{std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
    std::cout << "{\n"
        << "  \"network\": \"main\",\n"
        << "  \"headline\": \"" << HEADLINE << "\",\n"
        << "  \"time_utc\": \"2026-10-01T16:43:15Z\",\n"
        << "  \"time\": " << GENESIS_TIME << ",\n"
        << "  \"version\": 1,\n"
        << "  \"bits\": \"1e333300\",\n"
        << "  \"nonce\": " << block.nNonce << ",\n"
        << "  \"bootstrap_key\": \"" << BOOTSTRAP_KEY.GetHex() << "\",\n"
        << "  \"target\": \"" << ArithToUint256(target).GetHex() << "\",\n"
        << "  \"block_hash_sha256d\": \"" << block.GetHash().GetHex() << "\",\n"
        << "  \"pow_hash_randomx_v2\": \"" << pow_hash.GetHex() << "\",\n"
        << "  \"merkle_root\": \"" << block.hashMerkleRoot.GetHex() << "\",\n"
        << "  \"coinbase_txid\": \"" << block.vtx.front()->GetHash().ToString() << "\",\n"
        << "  \"coinbase_script_sig_bytes\": " << block.vtx.front()->vin.front().scriptSig.size() << ",\n"
        << "  \"outputs\": [\n"
        << "    {\"n\": 0, \"type\": 1, \"amount_connects\": \"50000000000000000\", \"pubkey\": \"" << PUBLIC_KEYS[0] << "\"},\n"
        << "    {\"n\": 1, \"type\": 1, \"amount_connects\": \"50000000000000000\", \"pubkey\": \"" << PUBLIC_KEYS[1] << "\"}\n"
        << "  ],\n"
        << "  \"header_hex\": \"" << HexStr(HeaderBytes(block)) << "\",\n"
        << "  \"coinbase_hex\": \"" << HexStr(tx_bytes) << "\",\n"
        << "  \"block_hex\": \"" << HexStr(block_bytes) << "\",\n"
        << "  \"verified_light_interpreter\": true,\n"
        << "  \"verified_fast\": " << (verify ? "false" : "true") << ",\n"
        << "  \"attempts\": " << attempts.load() << ",\n"
        << "  \"elapsed_seconds\": " << elapsed << "\n"
        << "}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
