// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/connectcoinkernel.h>
#include <kernel/connectcoinkernel_wrapper.h>
#include <util/byte_units.h>
#include <util/fs.h>

// Boost.Test's SIGSTKSZ alternate stack can be smaller than Linux requires on musl.
#define BOOST_TEST_DISABLE_ALT_STACK
#define BOOST_TEST_MODULE ConnectCoin Kernel Test Suite
#include <boost/test/included/unit_test.hpp>

#include <test/kernel/block_data.h>
#include <test/util/common.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cck;

std::string random_string(uint32_t length)
{
    const std::string chars = "0123456789"
                              "abcdefghijklmnopqrstuvwxyz"
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    static std::random_device rd;
    static std::default_random_engine dre{rd()};
    static std::uniform_int_distribution<> distribution(0, chars.size() - 1);

    std::string random;
    random.reserve(length);
    for (uint32_t i = 0; i < length; i++) {
        random += chars[distribution(dre)];
    }
    return random;
}

std::vector<std::byte> hex_string_to_byte_vec(std::string_view hex)
{
    std::vector<std::byte> bytes;
    bytes.reserve(hex.length() / 2);

    for (size_t i{0}; i < hex.length(); i += 2) {
        uint8_t byte_value;
        auto [ptr, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, byte_value, 16);

        if (ec != std::errc{} || ptr != hex.data() + i + 2) {
            throw std::invalid_argument("Invalid hex character");
        }
        bytes.push_back(static_cast<std::byte>(byte_value));
    }
    return bytes;
}

std::string byte_span_to_hex_string_reversed(std::span<const std::byte> bytes)
{
    std::ostringstream oss;

    // Iterate in reverse order
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(static_cast<uint8_t>(*it));
    }

    return oss.str();
}

constexpr std::string_view TEST_XONLY_PUBKEY{"17dcaae1908f91b39aa592755dcbba876610933e239c8630eb8bfc0ddff8b3b4"};
constexpr std::string_view TEST_P2PK_SCRIPT{"512017dcaae1908f91b39aa592755dcbba876610933e239c8630eb8bfc0ddff8b3b4"};
constexpr std::string_view TEST_COINBASE_TX{
    "020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff2802ce0024aa21a9ed1ab08ff674614d3b8be37cefffd867d117efa84bcfd4b4d9c9791a0c521efecefeffffff01d8d5546a740000000117dcaae1908f91b39aa592755dcbba876610933e239c8630eb8bfc0ddff8b3b401200000000000000000000000000000000000000000000000000000000000000000cd000000"};
constexpr std::string_view TEST_SPENDING_TX{
    "020000000001013f32810d70987d3eb431c22b5b453cc518c4471976c5d3e4d712b51331fcb3ad0000000000fdffffff0200e40b54020000000117dcaae1908f91b39aa592755dcbba876610933e239c8630eb8bfc0ddff8b3b428de9680e600000001555f1c7f8bc719cc53b59980ec0a2ab186b2438c6672330efc7e835c0447ca580140cd4256664792333e9e1234d94c9cf61f6d3af7e88882b807935c7e90f715d6c2b24e324b00f7eaf857cbbd745ed9530e122cc92c81a80a305d7c528648336c36cd000000"};

void check_equal(std::span<const std::byte> _actual, std::span<const std::byte> _expected, bool equal = true)
{
    std::span<const uint8_t> actual{reinterpret_cast<const unsigned char*>(_actual.data()), _actual.size()};
    std::span<const uint8_t> expected{reinterpret_cast<const unsigned char*>(_expected.data()), _expected.size()};
    BOOST_CHECK_EQUAL_COLLECTIONS(
        actual.begin(), actual.end(),
        expected.begin(), expected.end());
}

class TestLog
{
public:
    void LogMessage(std::string_view message)
    {
        std::cout << "kernel: " << message;
    }
};

struct TestDirectory {
    fs::path m_directory;
    TestDirectory(std::string directory_name)
        : m_directory{fs::path{fs::temp_directory_path()} / fs::u8path(directory_name + "_🌽_" + random_string(16))}
    {
        fs::create_directories(m_directory);
    }

    ~TestDirectory()
    {
        fs::remove_all(m_directory);
    }
};

class TestKernelNotifications : public KernelNotifications
{
public:
    void HeaderTipHandler(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override
    {
        BOOST_CHECK_GT(timestamp, 0);
    }

    void FlushErrorHandler(std::string_view error) override
    {
        std::cout << error << std::endl;
    }

    void FatalErrorHandler(std::string_view error) override
    {
        std::cout << error << std::endl;
    }
};

class TestValidationInterface : public ValidationInterface
{
public:
    std::optional<std::vector<std::byte>> m_expected_valid_block = std::nullopt;

    void BlockChecked(Block block, BlockValidationStateView state) override
    {
        if (m_expected_valid_block.has_value()) {
            auto ser_block{block.ToBytes()};
            check_equal(m_expected_valid_block.value(), ser_block);
        }

        auto mode{state.GetValidationMode()};
        switch (mode) {
        case ValidationMode::VALID: {
            std::cout << "Valid block" << std::endl;
            return;
        }
        case ValidationMode::INVALID: {
            std::cout << "Invalid block: ";
            auto result{state.GetBlockValidationResult()};
            switch (result) {
            case BlockValidationResult::UNSET:
                std::cout << "initial value. Block has not yet been rejected" << std::endl;
                break;
            case BlockValidationResult::HEADER_LOW_WORK:
                std::cout << "the block header may be on a too-little-work chain" << std::endl;
                break;
            case BlockValidationResult::CONSENSUS:
                std::cout << "invalid by consensus rules (excluding any below reasons)" << std::endl;
                break;
            case BlockValidationResult::CACHED_INVALID:
                std::cout << "this block was cached as being invalid and we didn't store the reason why" << std::endl;
                break;
            case BlockValidationResult::INVALID_HEADER:
                std::cout << "invalid proof of work or time too old" << std::endl;
                break;
            case BlockValidationResult::MUTATED:
                std::cout << "the block's data didn't match the data committed to by the PoW" << std::endl;
                break;
            case BlockValidationResult::MISSING_PREV:
                std::cout << "We don't have the previous block the checked one is built on" << std::endl;
                break;
            case BlockValidationResult::INVALID_PREV:
                std::cout << "A block this one builds on is invalid" << std::endl;
                break;
            case BlockValidationResult::TIME_FUTURE:
                std::cout << "block timestamp was > 2 hours in the future (or our clock is bad)" << std::endl;
                break;
            }
            return;
        }
        case ValidationMode::INTERNAL_ERROR: {
            std::cout << "Internal error" << std::endl;
            return;
        }
        }
    }

    void BlockConnected(Block block, BlockTreeEntry entry) override
    {
        std::cout << "Block connected." << std::endl;
    }

    void PowValidBlock(BlockTreeEntry entry, Block block) override
    {
        std::cout << "Block passed pow verification" << std::endl;
    }

    void BlockDisconnected(Block block, BlockTreeEntry entry) override
    {
        std::cout << "Block disconnected." << std::endl;
    }
};

template <typename T>
concept HasToBytes = requires(T t) {
    { t.ToBytes() } -> std::convertible_to<std::span<const std::byte>>;
};

template <typename T>
void CheckHandle(T object, T distinct_object)
{
    BOOST_CHECK(object.get() != nullptr);
    BOOST_CHECK(distinct_object.get() != nullptr);
    BOOST_CHECK(object.get() != distinct_object.get());

    if constexpr (HasToBytes<T>) {
        const auto object_bytes = object.ToBytes();
        const auto distinct_bytes = distinct_object.ToBytes();
        BOOST_CHECK(!std::ranges::equal(object_bytes, distinct_bytes));
    }

    // Copy constructor
    T object2(distinct_object);
    BOOST_CHECK_NE(distinct_object.get(), object2.get());
    if constexpr (HasToBytes<T>) {
        check_equal(distinct_object.ToBytes(), object2.ToBytes());
    }

    // Copy assignment
    T object3{distinct_object};
    object2 = object3;
    BOOST_CHECK_NE(object3.get(), object2.get());
    if constexpr (HasToBytes<T>) {
        check_equal(object3.ToBytes(), object2.ToBytes());
    }

    // Move constructor
    auto* original_ptr = object2.get();
    T object4{std::move(object2)};
    BOOST_CHECK_EQUAL(object4.get(), original_ptr);
    BOOST_CHECK_EQUAL(object2.get(), nullptr); // NOLINT(bugprone-use-after-move)
    if constexpr (HasToBytes<T>) {
        check_equal(object4.ToBytes(), object3.ToBytes());
    }

    // Move assignment
    original_ptr = object4.get();
    object2 = std::move(object4);
    BOOST_CHECK_EQUAL(object2.get(), original_ptr);
    BOOST_CHECK_EQUAL(object4.get(), nullptr); // NOLINT(bugprone-use-after-move)
    if constexpr (HasToBytes<T>) {
        check_equal(object2.ToBytes(), object3.ToBytes());
    }

    // Self move-assignment must not destroy the held resource.
    // Use a reference to avoid -Wself-move warnings.
    original_ptr = object2.get();
    auto& object2_ref = object2;
    object2 = std::move(object2_ref);
    BOOST_CHECK_EQUAL(object2.get(), original_ptr);
    if constexpr (HasToBytes<T>) {
        check_equal(object2.ToBytes(), object3.ToBytes());
    }
}

template <typename RangeType>
    requires std::ranges::random_access_range<RangeType>
void CheckRange(const RangeType& range, size_t expected_size)
{
    using value_type = std::ranges::range_value_t<RangeType>;

    BOOST_CHECK_EQUAL(range.size(), expected_size);
    BOOST_REQUIRE(range.size() > 0); // Some checks below assume a non-empty range
    BOOST_REQUIRE(!range.empty());

    BOOST_CHECK(range.begin() != range.end());
    BOOST_CHECK_EQUAL(std::distance(range.begin(), range.end()), static_cast<std::ptrdiff_t>(expected_size));
    BOOST_CHECK(range.cbegin() == range.begin());
    BOOST_CHECK(range.cend() == range.end());

    for (size_t i = 0; i < range.size(); ++i) {
        BOOST_CHECK_EQUAL(range[i].get(), (*(range.begin() + i)).get());
    }

    BOOST_CHECK_THROW(range.at(expected_size), std::out_of_range);

    BOOST_CHECK_EQUAL(range.front().get(), range[0].get());
    BOOST_CHECK_EQUAL(range.back().get(), range[expected_size - 1].get());

    auto it = range.begin();
    auto it_copy = it;
    ++it;
    BOOST_CHECK(it != it_copy);
    --it;
    BOOST_CHECK(it == it_copy);
    it = range.begin();
    auto old_it = it++;
    BOOST_CHECK(old_it == range.begin());
    BOOST_CHECK(it == range.begin() + 1);
    old_it = it--;
    BOOST_CHECK(old_it == range.begin() + 1);
    BOOST_CHECK(it == range.begin());

    it = range.begin();
    it += 2;
    BOOST_CHECK(it == range.begin() + 2);
    it -= 2;
    BOOST_CHECK(it == range.begin());

    BOOST_CHECK(range.begin() < range.end());
    BOOST_CHECK(range.begin() <= range.end());
    BOOST_CHECK(range.end() > range.begin());
    BOOST_CHECK(range.end() >= range.begin());
    BOOST_CHECK(range.begin() == range.begin());

    BOOST_CHECK_EQUAL(range.begin()[0].get(), range[0].get());

    size_t count = 0;
    for (auto rit = range.end(); rit != range.begin();) {
        --rit;
        ++count;
    }
    BOOST_CHECK_EQUAL(count, expected_size);

    std::vector<value_type> collected;
    for (const auto& elem : range) {
        collected.push_back(elem);
    }
    BOOST_CHECK_EQUAL(collected.size(), expected_size);

    BOOST_CHECK_EQUAL(std::ranges::size(range), expected_size);

    it = range.begin();
    auto it2 = 1 + it;
    BOOST_CHECK(it2 == it + 1);
}

BOOST_AUTO_TEST_CASE(cck_transaction_tests)
{
    auto tx_data{hex_string_to_byte_vec(TEST_SPENDING_TX)};
    auto tx{Transaction{tx_data}};
    auto tx_data_2{hex_string_to_byte_vec(TEST_COINBASE_TX)};
    auto tx2{Transaction{tx_data_2}};
    CheckHandle(tx, tx2);

    auto invalid_data = hex_string_to_byte_vec("012300");
    BOOST_CHECK_THROW(Transaction{invalid_data}, std::runtime_error);
    auto empty_data = hex_string_to_byte_vec("");
    BOOST_CHECK_THROW(Transaction{empty_data}, std::runtime_error);

    BOOST_CHECK_EQUAL(tx.CountOutputs(), 2);
    BOOST_CHECK_EQUAL(tx.CountInputs(), 1);
    BOOST_CHECK_EQUAL(tx.GetLocktime(), 205);
    auto broken_tx_data{std::span<std::byte>{tx_data.begin(), tx_data.begin() + 10}};
    BOOST_CHECK_THROW(Transaction{broken_tx_data}, std::runtime_error);
    auto input{tx.GetInput(0)};
    BOOST_CHECK_EQUAL(input.GetSequence(), 0xfffffffd);
    auto output{tx.GetOutput(tx.CountOutputs() - 1)};
    BOOST_CHECK_EQUAL(output.Amount(), 989999849000);
    auto script_pubkey{output.GetScriptPubkey()};
    {
        auto tx_new{Transaction{tx_data}};
        // This is safe, because we now use copy assignment
        TransactionOutput output = tx_new.GetOutput(tx_new.CountOutputs() - 1);
        ScriptPubkey script = output.GetScriptPubkey();

        TransactionOutputView output2 = tx_new.GetOutput(tx_new.CountOutputs() - 1);
        BOOST_CHECK_NE(output.get(), output2.get());
        BOOST_CHECK_EQUAL(output.Amount(), output2.Amount());
        TransactionOutput output3 = output2;
        BOOST_CHECK_NE(output3.get(), output2.get());
        BOOST_CHECK_EQUAL(output3.Amount(), output2.Amount());

        // Non-owned view
        ScriptPubkeyView script2 = output.GetScriptPubkey();
        BOOST_CHECK_NE(script.get(), script2.get());
        check_equal(script.ToBytes(), script2.ToBytes());

        // Non-owned to owned
        ScriptPubkey script3 = script2;
        BOOST_CHECK_NE(script3.get(), script2.get());
        check_equal(script3.ToBytes(), script2.ToBytes());
    }
    BOOST_CHECK_EQUAL(output.Amount(), 989999849000);

    auto tx_roundtrip{Transaction{tx.ToBytes()}};
    check_equal(tx_roundtrip.ToBytes(), tx_data);

    // The following code is unsafe, but left here to show limitations of the
    // API, because we preserve the output view beyond the lifetime of the
    // transaction. The view type wrapper should make this clear to the user.
    // auto get_output = [&]() -> TransactionOutputView {
    //     auto tx{Transaction{tx_data}};
    //     return tx.GetOutput(0);
    // };
    // auto output_new = get_output();
    // BOOST_CHECK_EQUAL(output_new.Amount(), 20737411);

    int64_t total_amount{0};
    for (const auto output : tx.Outputs()) {
        total_amount += output.Amount();
    }
    BOOST_CHECK_EQUAL(total_amount, 999999849000);

    auto amount = *(tx.Outputs() | std::ranges::views::filter([](const auto& output) {
                        return output.Amount() == 989999849000;
                    }) |
                    std::views::transform([](const auto& output) {
                        return output.Amount();
                    })).begin();
    BOOST_REQUIRE(amount);
    BOOST_CHECK_EQUAL(amount, 989999849000);

    CheckRange(tx.Outputs(), tx.CountOutputs());

    ScriptPubkey script_pubkey_roundtrip{script_pubkey.ToBytes()};
    check_equal(script_pubkey_roundtrip.ToBytes(), script_pubkey.ToBytes());
}

BOOST_AUTO_TEST_CASE(cck_script_pubkey)
{
    auto script_data{hex_string_to_byte_vec("76a9144bfbaf6afb76cc5771bc6404810d1cc041a6933988ac")};
    std::vector<std::byte> script_data_2 = script_data;
    script_data_2.push_back(std::byte{0x51});
    ScriptPubkey script{script_data};
    ScriptPubkey script2{script_data_2};
    CheckHandle(script, script2);

    std::span<std::byte> empty_data{};
    ScriptPubkey empty_script{empty_data};
    CheckHandle(script, empty_script);
}

BOOST_AUTO_TEST_CASE(cck_transaction_output)
{
    ScriptPubkey script{hex_string_to_byte_vec(TEST_P2PK_SCRIPT)};
    TransactionOutput output{script, 1};
    TransactionOutput output2{script, 2};
    CheckHandle(output, output2);
}

BOOST_AUTO_TEST_CASE(cck_transaction_input)
{
    Transaction tx{hex_string_to_byte_vec(TEST_SPENDING_TX)};
    Transaction coinbase{hex_string_to_byte_vec(TEST_COINBASE_TX)};
    TransactionInput input_0 = tx.GetInput(0);
    TransactionInput input_1 = coinbase.GetInput(0);
    CheckHandle(input_0, input_1);
    CheckRange(tx.Inputs(), tx.CountInputs());
    OutPoint point_0 = input_0.OutPoint();
    OutPoint point_1 = input_1.OutPoint();
    CheckHandle(point_0, point_1);

    WitnessStackView ws_0 = input_0.GetWitnessStack();
    BOOST_CHECK_EQUAL(ws_0.CountItems(), 1);
    BOOST_CHECK_EQUAL(ws_0.GetItem(0).size(), 64);
    BOOST_CHECK(input_0.GetScriptSig().empty());

    WitnessStackView ws = input_1.GetWitnessStack();
    BOOST_CHECK_EQUAL(ws.CountItems(), 1);
    BOOST_CHECK_EQUAL(ws.GetItem(0).size(), 32);
    auto items = ws.Items();
    BOOST_CHECK_EQUAL(items.size(), 1);
    for (size_t i = 0; i < items.size(); ++i) {
        BOOST_CHECK(items[i] == ws.GetItem(i));
    }
    WitnessStack owned_ws_0{ws_0};
    WitnessStack owned_ws{ws};
    CheckHandle(owned_ws_0, owned_ws);
}

BOOST_AUTO_TEST_CASE(cck_precomputed_txdata) {
    auto tx_data{hex_string_to_byte_vec(TEST_SPENDING_TX)};
    auto tx{Transaction{tx_data}};
    auto tx_data_2{hex_string_to_byte_vec(TEST_COINBASE_TX)};
    auto tx2{Transaction{tx_data_2}};
    auto precomputed_txdata{PrecomputedTransactionData{
        /*tx_to=*/tx,
        /*spent_outputs=*/{},
    }};
    auto precomputed_txdata_2{PrecomputedTransactionData{
        /*tx_to=*/tx2,
        /*spent_outputs=*/{},
    }};
    CheckHandle(precomputed_txdata, precomputed_txdata_2);
}

BOOST_AUTO_TEST_CASE(cck_script_verify_tests)
{
    constexpr int64_t spent_amount{1'000'000'000'000};
    ScriptPubkey spent_script_pubkey{hex_string_to_byte_vec(TEST_P2PK_SCRIPT)};
    Transaction spending_tx{hex_string_to_byte_vec(TEST_SPENDING_TX)};
    std::vector<TransactionOutput> spent_outputs;
    spent_outputs.emplace_back(spent_script_pubkey, spent_amount);
    PrecomputedTransactionData precomputed_txdata{spending_tx, spent_outputs};

    auto status{ScriptVerifyStatus::OK};
    BOOST_CHECK(!spent_script_pubkey.Verify(spent_amount, spending_tx, nullptr, 0,
                                            ScriptVerificationFlags::ALL, status));
    BOOST_CHECK(status == ScriptVerifyStatus::ERROR_SPENT_OUTPUTS_REQUIRED);

    BOOST_CHECK(spent_script_pubkey.Verify(spent_amount, spending_tx, &precomputed_txdata, 0,
                                           ScriptVerificationFlags::ALL, status));
    BOOST_CHECK(status == ScriptVerifyStatus::OK);

    BOOST_CHECK(!spent_script_pubkey.Verify(spent_amount - 1, spending_tx, &precomputed_txdata, 0,
                                            ScriptVerificationFlags::ALL, status));
    BOOST_CHECK(status == ScriptVerifyStatus::OK);
}

BOOST_AUTO_TEST_CASE(logging_tests)
{
    cck_LoggingOptions logging_options = {
        .log_timestamps = true,
        .log_time_micros = true,
        .log_threadnames = false,
        .log_sourcelocations = false,
        .always_print_category_levels = true,
    };

    logging_set_options(logging_options);
    logging_set_level_category(LogCategory::BENCH, LogLevel::TRACE_LEVEL);
    logging_disable_category(LogCategory::BENCH);
    logging_enable_category(LogCategory::VALIDATION);
    logging_disable_category(LogCategory::VALIDATION);

    // Check that connecting, connecting another, and then disconnecting and connecting a logger again works.
    {
        logging_set_level_category(LogCategory::KERNEL, LogLevel::TRACE_LEVEL);
        logging_enable_category(LogCategory::KERNEL);
        Logger logger{std::make_unique<TestLog>()};
        Logger logger_2{std::make_unique<TestLog>()};
    }
    Logger logger{std::make_unique<TestLog>()};
}

BOOST_AUTO_TEST_CASE(cck_chainparams_tests)
{
    ChainParams params_signet{ChainType::SIGNET};
    ChainParams params_signet_challenge{hex_string_to_byte_vec("51")};
    CheckHandle(params_signet, params_signet_challenge);
}

BOOST_AUTO_TEST_CASE(cck_context_tests)
{
    { // test default context
        Context context{};
        Context context2{};
        CheckHandle(context, context2);
    }

    { // test with context options, but not options set
        ContextOptions options{};
        Context context{options};
    }

    { // test with context options
        ContextOptions options{};
        ChainParams params{ChainType::MAINNET};
        ChainParams regtest_params{ChainType::REGTEST};
        CheckHandle(params, regtest_params);
        options.SetChainParams(params);
        options.SetNotifications(std::make_shared<TestKernelNotifications>());
        Context context{options};
    }
}

BOOST_AUTO_TEST_CASE(cck_block_header_tests)
{
    // Block header format: version(4) + prev_hash(32) + merkle_root(32) + timestamp(4) + bits(4) + nonce(4) = 80 bytes
    BlockHeader header_0{hex_string_to_byte_vec("00e07a26beaaeee2e71d7eb19279545edbaf15de0999983626ec00000000000000000000579cf78b65229bfb93f4a11463af2eaa5ad91780f27f5d147a423bea5f7e4cdf2a47e268b4dd01173a9662ee")};
    BOOST_CHECK_EQUAL(byte_span_to_hex_string_reversed(header_0.Hash().ToBytes()), "00000000000000000000325c7e14a4ee3b4fcb2343089a839287308a0ddbee4f");
    BlockHeader header_1{hex_string_to_byte_vec("00c00020e7cb7b4de21d26d55bd384017b8bb9333ac3b2b55bed00000000000000000000d91b4484f801b99f03d36b9d26cfa83420b67f81da12d7e6c1e7f364e743c5ba9946e268b4dd011799c8533d")};
    CheckHandle(header_0, header_1);

    // Test all header field accessors using mainnet block 1
    auto mainnet_block_1_header = hex_string_to_byte_vec(MAINNET_BLOCK_1_DATA);
    mainnet_block_1_header.resize(80);
    BlockHeader header{mainnet_block_1_header};
    BOOST_CHECK_EQUAL(header.Version(), 4);
    BOOST_CHECK_EQUAL(header.Timestamp(), 1787596841);
    BOOST_CHECK_EQUAL(header.Bits(), 0x1f00ffff);
    BOOST_CHECK_EQUAL(header.Nonce(), 69871);
    BOOST_CHECK_EQUAL(byte_span_to_hex_string_reversed(header.Hash().ToBytes()), "7bfacd75f5e2f90f898f22e2619551d63de51d33e1ed0bc2e8fe1e8c7e751339");
    auto prev_hash = header.PrevHash();
    BOOST_CHECK_EQUAL(byte_span_to_hex_string_reversed(prev_hash.ToBytes()), "8b6373205ad2b6314f2937cebacfc143af9eb6183162c24fb19cdf382ff576c5");

    // Test round-trip serialization of block header
    auto header_roundtrip{BlockHeader{header.ToBytes()}};
    check_equal(header_roundtrip.ToBytes(), mainnet_block_1_header);

    auto raw_block = hex_string_to_byte_vec(MAINNET_BLOCK_1_DATA);
    Block block{raw_block};
    BlockHeader block_header{block.GetHeader()};
    BOOST_CHECK_EQUAL(block_header.Version(), 4);
    BOOST_CHECK_EQUAL(block_header.Timestamp(), 1787596841);
    BOOST_CHECK_EQUAL(block_header.Bits(), 0x1f00ffff);
    BOOST_CHECK_EQUAL(block_header.Nonce(), 69871);
    BOOST_CHECK_EQUAL(byte_span_to_hex_string_reversed(block_header.Hash().ToBytes()), "7bfacd75f5e2f90f898f22e2619551d63de51d33e1ed0bc2e8fe1e8c7e751339");

    // Verify header from block serializes to first 80 bytes of raw block
    auto block_header_bytes = block_header.ToBytes();
    BOOST_CHECK_EQUAL(block_header_bytes.size(), 80);
    check_equal(block_header_bytes, std::span<const std::byte>(raw_block.data(), 80));
}

BOOST_AUTO_TEST_CASE(cck_block)
{
    Block block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[0])};
    Block block_100{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[100])};
    CheckHandle(block, block_100);
    Block block_tx{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[205])};
    CheckRange(block_tx.Transactions(), block_tx.CountTransactions());
    auto transactions{block_tx.Transactions()};
    auto transactions_copy{transactions};
    BOOST_CHECK(transactions.begin() == transactions_copy.begin());
    BOOST_CHECK(transactions.begin() == block_tx.Transactions().begin());
    auto transaction_it{transactions.begin()};
    BOOST_CHECK((*transaction_it).Txid() == block_tx.GetTransaction(0).Txid());
    auto invalid_data = hex_string_to_byte_vec("012300");
    BOOST_CHECK_THROW(Block{invalid_data}, std::runtime_error);
    auto empty_data = hex_string_to_byte_vec("");
    BOOST_CHECK_THROW(Block{empty_data}, std::runtime_error);
}

Context create_context(std::shared_ptr<TestKernelNotifications> notifications, ChainType chain_type, std::shared_ptr<TestValidationInterface> validation_interface = nullptr)
{
    ContextOptions options{};
    ChainParams params{chain_type};
    options.SetChainParams(params);
    options.SetNotifications(notifications);
    if (validation_interface) {
        options.SetValidationInterface(validation_interface);
    }
    auto context{Context{options}};
    return context;
}

BOOST_AUTO_TEST_CASE(cck_chainman_tests)
{
    Logger logger{std::make_unique<TestLog>()};
    auto test_directory{TestDirectory{"chainman_test_connectcoin_kernel"}};

    { // test with default context
        Context context{};
        ChainstateManagerOptions chainman_opts{context, PathToString(test_directory.m_directory), PathToString(test_directory.m_directory / "blocks")};
        ChainMan chainman{context, chainman_opts};
    }

    { // test with default context options
        ContextOptions options{};
        Context context{options};
        ChainstateManagerOptions chainman_opts{context, PathToString(test_directory.m_directory), PathToString(test_directory.m_directory / "blocks")};
        ChainMan chainman{context, chainman_opts};
    }
    { // null or empty data_directory or blocks_directory are not allowed
        Context context{};
        auto valid_dir{PathToString(test_directory.m_directory)};
        std::vector<std::pair<std::string_view, std::string_view>> illegal_cases{
            {"", valid_dir},
            {valid_dir, {nullptr, 0}},
            {"", ""},
            {{nullptr, 0}, {nullptr, 0}},
        };
        for (auto& [data_dir, blocks_dir] : illegal_cases) {
            BOOST_CHECK_THROW(ChainstateManagerOptions(context, data_dir, blocks_dir),
                              std::runtime_error);
        };
    }

    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::MAINNET)};

    ChainstateManagerOptions chainman_opts{context, PathToString(test_directory.m_directory), PathToString(test_directory.m_directory / "blocks")};
    chainman_opts.SetWorkerThreads(4);
    BOOST_CHECK(!chainman_opts.SetDatabaseCacheBytes(4_MiB - 1));
    if constexpr (sizeof(void*) == 4) BOOST_CHECK(!chainman_opts.SetDatabaseCacheBytes(2_GiB));
    BOOST_CHECK(chainman_opts.SetDatabaseCacheBytes(4_MiB));
    BOOST_CHECK(!chainman_opts.SetWipeDbs(/*wipe_block_tree=*/true, /*wipe_chainstate=*/false));
    BOOST_CHECK(chainman_opts.SetWipeDbs(/*wipe_block_tree=*/true, /*wipe_chainstate=*/true));
    BOOST_CHECK(chainman_opts.SetWipeDbs(/*wipe_block_tree=*/false, /*wipe_chainstate=*/true));
    BOOST_CHECK(chainman_opts.SetWipeDbs(/*wipe_block_tree=*/false, /*wipe_chainstate=*/false));
    ChainMan chainman{context, chainman_opts};
}

std::unique_ptr<ChainMan> create_chainman(TestDirectory& test_directory,
                                          bool reindex,
                                          bool wipe_chainstate,
                                          bool block_tree_db_in_memory,
                                          bool chainstate_db_in_memory,
                                          Context& context)
{
    ChainstateManagerOptions chainman_opts{context, PathToString(test_directory.m_directory), PathToString(test_directory.m_directory / "blocks")};

    if (reindex) {
        chainman_opts.SetWipeDbs(/*wipe_block_tree=*/reindex, /*wipe_chainstate=*/reindex);
    }
    if (wipe_chainstate) {
        chainman_opts.SetWipeDbs(/*wipe_block_tree=*/false, /*wipe_chainstate=*/wipe_chainstate);
    }
    if (block_tree_db_in_memory) {
        chainman_opts.UpdateBlockTreeDbInMemory(block_tree_db_in_memory);
    }
    if (chainstate_db_in_memory) {
        chainman_opts.UpdateChainstateDbInMemory(chainstate_db_in_memory);
    }

    auto chainman{std::make_unique<ChainMan>(context, chainman_opts)};
    return chainman;
}

void chainman_reindex_test(TestDirectory& test_directory)
{
    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::MAINNET)};
    auto chainman{create_chainman(
        test_directory, /*reindex=*/true, /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};

    std::vector<std::string> import_files;
    BOOST_CHECK(chainman->ImportBlocks(import_files));

    // Sanity check some block retrievals
    auto chain{chainman->GetChain()};
    BOOST_CHECK_THROW(chain.GetByHeight(1000), std::runtime_error);
    auto genesis_index{chain.Entries().front()};
    BOOST_CHECK(!genesis_index.GetPrevious());
    auto genesis_block_raw{chainman->ReadBlock(genesis_index).value().ToBytes()};
    auto first_index{chain.GetByHeight(0)};
    auto first_block_raw{chainman->ReadBlock(genesis_index).value().ToBytes()};
    check_equal(genesis_block_raw, first_block_raw);
    auto height{first_index.GetHeight()};
    BOOST_CHECK_EQUAL(height, 0);

    auto next_index{chain.GetByHeight(first_index.GetHeight() + 1)};
    BOOST_CHECK(chain.Contains(next_index));
    auto next_block_data{chainman->ReadBlock(next_index).value().ToBytes()};
    auto tip_index{chain.Entries().back()};
    auto tip_block_data{chainman->ReadBlock(tip_index).value().ToBytes()};
    auto second_index{chain.GetByHeight(1)};
    auto second_block{chainman->ReadBlock(second_index).value()};
    auto second_block_data{second_block.ToBytes()};
    auto second_height{second_index.GetHeight()};
    BOOST_CHECK_EQUAL(second_height, 1);
    check_equal(next_block_data, tip_block_data);
    check_equal(next_block_data, second_block_data);

    auto second_hash{second_index.GetHash()};
    auto another_second_index{chainman->GetBlockTreeEntry(second_hash)};
    BOOST_CHECK(another_second_index);
    auto another_second_height{another_second_index->GetHeight()};
    auto second_block_hash{second_block.GetHash()};
    check_equal(second_block_hash.ToBytes(), second_hash.ToBytes());
    BOOST_CHECK_EQUAL(second_height, another_second_height);
}

void chainman_reindex_chainstate_test(TestDirectory& test_directory)
{
    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::MAINNET)};
    auto chainman{create_chainman(
        test_directory, /*reindex=*/false, /*wipe_chainstate=*/true,
        /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};

    std::vector<std::string> import_files;
    import_files.push_back(PathToString(test_directory.m_directory / "blocks" / "blk00000.dat"));
    BOOST_CHECK(chainman->ImportBlocks(import_files));
}

void chainman_mainnet_validation_test(TestDirectory& test_directory)
{
    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto validation_interface{std::make_shared<TestValidationInterface>()};
    auto context{create_context(notifications, ChainType::MAINNET, validation_interface)};
    auto chainman{create_chainman(
        test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};

    // mainnet block 1
    auto raw_block = hex_string_to_byte_vec(MAINNET_BLOCK_1_DATA);
    Block block{raw_block};
    BlockHeader header{block.GetHeader()};
    TransactionView tx{block.GetTransaction(block.CountTransactions() - 1)};
    BOOST_CHECK_EQUAL(byte_span_to_hex_string_reversed(tx.Txid().ToBytes()), "b6d6f00f0c1bf525e2998e042cc270aa0b8fb3645da3559e0857bec8bf79d08a");
    BOOST_CHECK_EQUAL(header.Version(), 4);
    BOOST_CHECK_EQUAL(header.Timestamp(), 1787596841);
    BOOST_CHECK_EQUAL(header.Bits(), 0x1f00ffff);
    BOOST_CHECK_EQUAL(header.Nonce(), 69871);
    BOOST_CHECK_EQUAL(tx.CountInputs(), 1);
    Transaction tx2 = tx;
    BOOST_CHECK_EQUAL(tx2.CountInputs(), 1);
    for (auto transaction : block.Transactions()) {
        BOOST_CHECK_EQUAL(transaction.CountInputs(), 1);
    }
    auto output_counts = *(block.Transactions() | std::views::transform([](const auto& tx) {
                               return tx.CountOutputs();
                           })).begin();
    BOOST_CHECK_EQUAL(output_counts, 1);

    validation_interface->m_expected_valid_block.emplace(raw_block);
    auto ser_block{block.ToBytes()};
    check_equal(ser_block, raw_block);
    bool new_block = false;
    BOOST_CHECK(chainman->ProcessBlock(block, &new_block));
    BOOST_CHECK(new_block);

    validation_interface->m_expected_valid_block = std::nullopt;
    new_block = false;
    Block invalid_block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[REGTEST_BLOCK_DATA.size() - 1])};
    BOOST_CHECK(!chainman->ProcessBlock(invalid_block, &new_block));
    BOOST_CHECK(!new_block);

    auto chain{chainman->GetChain()};
    BOOST_CHECK_EQUAL(chain.Height(), 1);
    auto tip{chain.Entries().back()};
    auto read_block{chainman->ReadBlock(tip)};
    BOOST_REQUIRE(read_block);
    check_equal(read_block.value().ToBytes(), raw_block);

    // Check that we can read the previous block
    BlockTreeEntry tip_2{*tip.GetPrevious()};
    Block read_block_2{*chainman->ReadBlock(tip_2)};
    BOOST_CHECK_EQUAL(chainman->ReadBlockSpentOutputs(tip_2).Count(), 0);
    BOOST_CHECK_EQUAL(chainman->ReadBlockSpentOutputs(tip).Count(), 0);

    // It should be an error if we go another block back, since the genesis has no ancestor
    BOOST_CHECK(!tip_2.GetPrevious());

    // If we try to validate it again, it should be a duplicate
    BOOST_CHECK(chainman->ProcessBlock(block, &new_block));
    BOOST_CHECK(!new_block);
}

BOOST_AUTO_TEST_CASE(cck_check_block_context_free)
{
    constexpr size_t MERKLE_ROOT_OFFSET{4 + 32};
    constexpr size_t NBITS_OFFSET{4 + 32 + 32 + 4};
    constexpr size_t COINBASE_PREVOUT_N_OFFSET{4 + 32 + 32 + 4 + 4 + 4 + 1 + 4 + 1 + 32};

    // Mainnet block 1
    auto raw_block = hex_string_to_byte_vec(MAINNET_BLOCK_1_DATA);

    // Context-free block checks still need consensus params for the optional
    // proof-of-work validation path.
    ChainParams mainnet_params{ChainType::MAINNET};
    auto consensus_params = mainnet_params.GetConsensusParams();

    Block block{raw_block};
    BlockValidationState state;

    BOOST_CHECK(block.Check(consensus_params, BlockCheckFlags::BASE, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);

    BOOST_CHECK_MESSAGE(block.Check(consensus_params, BlockCheckFlags::ALL, state),
                        "block result=" << static_cast<int>(state.GetBlockValidationResult()));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);

    // Exercise the context-aware C ABI on the same block object twice. A
    // successful check must not let CBlock::fChecked bypass a later RandomX
    // check with a different epoch key.
    const auto bootstrap_key_hex = hex_string_to_byte_vec("d91b262aecaac2c4868b2cbe1563538f107c33fbee8c5d373bdaa8e551567fe5");
    std::array<std::byte, 32> bootstrap_key_bytes;
    for (size_t i{0}; i < bootstrap_key_bytes.size(); ++i) {
        bootstrap_key_bytes[i] = bootstrap_key_hex[bootstrap_key_bytes.size() - i - 1];
    }
    BlockHash bootstrap_key{bootstrap_key_bytes};
    BlockHashView bootstrap_key_view{bootstrap_key.get()};
    BOOST_CHECK_MESSAGE(block.Check(consensus_params, BlockCheckFlags::ALL, bootstrap_key_view, /*block_height=*/1, state),
                        "explicit-key block result=" << static_cast<int>(state.GetBlockValidationResult()));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);

    std::array<std::byte, 32> wrong_key_bytes{};
    BlockHash wrong_key{wrong_key_bytes};
    BlockHashView wrong_key_view{wrong_key.get()};
    BOOST_CHECK(!block.Check(consensus_params, BlockCheckFlags::POW, wrong_key_view, /*block_height=*/1, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::INVALID);

    BOOST_CHECK(!block.Check(consensus_params, BlockCheckFlags::POW, bootstrap_key_view, /*block_height=*/-1, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::INTERNAL_ERROR);

    auto bad_merkle_block_data = raw_block;
    bad_merkle_block_data[MERKLE_ROOT_OFFSET] ^= std::byte{0x01};
    Block bad_merkle_block{bad_merkle_block_data};

    BOOST_CHECK(!bad_merkle_block.Check(consensus_params, BlockCheckFlags::MERKLE, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::INVALID);
    BOOST_CHECK(state.GetBlockValidationResult() == BlockValidationResult::MUTATED);

    BOOST_CHECK(bad_merkle_block.Check(consensus_params, BlockCheckFlags::BASE, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);

    auto bad_pow_block_data = raw_block;
    bad_pow_block_data[NBITS_OFFSET + 3] = std::byte{0x1c};
    Block bad_pow_block{bad_pow_block_data};

    BOOST_CHECK(!bad_pow_block.Check(consensus_params, BlockCheckFlags::POW, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::INVALID);
    BOOST_CHECK(state.GetBlockValidationResult() == BlockValidationResult::INVALID_HEADER);

    BOOST_CHECK(bad_pow_block.Check(consensus_params, BlockCheckFlags::MERKLE, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);

    auto bad_base_block_data = raw_block;
    bad_base_block_data[COINBASE_PREVOUT_N_OFFSET] = std::byte{0x00};
    Block bad_base_block{bad_base_block_data};

    BOOST_CHECK(!bad_base_block.Check(consensus_params, BlockCheckFlags::BASE, state));
    BOOST_CHECK(state.GetValidationMode() == ValidationMode::INVALID);
    BOOST_CHECK(state.GetBlockValidationResult() == BlockValidationResult::CONSENSUS);

    // Test with invalid truncated block data.
    auto truncated_block_data = raw_block;
    truncated_block_data.resize(80);
    BOOST_CHECK_EXCEPTION(Block{truncated_block_data}, std::runtime_error,
                          HasReason{"failed to instantiate cck object"});
}

BOOST_AUTO_TEST_CASE(cck_chainman_mainnet_tests)
{
    auto test_directory{TestDirectory{"mainnet_test_connectcoin_kernel"}};
    chainman_mainnet_validation_test(test_directory);
    chainman_reindex_test(test_directory);
    chainman_reindex_chainstate_test(test_directory);
}

BOOST_AUTO_TEST_CASE(cck_block_hash_tests)
{
    std::array<std::byte, 32> test_hash;
    std::array<std::byte, 32> test_hash_2;
    for (int i = 0; i < 32; ++i) {
        test_hash[i] = static_cast<std::byte>(i);
        test_hash_2[i] = static_cast<std::byte>(i + 1);
    }
    BlockHash block_hash{test_hash};
    BlockHash block_hash_2{test_hash_2};
    BOOST_CHECK(block_hash != block_hash_2);
    BOOST_CHECK(block_hash == block_hash);
    CheckHandle(block_hash, block_hash_2);
}

BOOST_AUTO_TEST_CASE(cck_block_tree_entry_tests)
{
    auto test_directory{TestDirectory{"block_tree_entry_test_connectcoin_kernel"}};
    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::REGTEST)};
    auto chainman{create_chainman(
        test_directory,
        /*reindex=*/false,
        /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/true,
        /*chainstate_db_in_memory=*/true,
        context)};

    // Process a couple of blocks
    for (size_t i{0}; i < 3; i++) {
        Block block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[i])};
        bool new_block{false};
        chainman->ProcessBlock(block, &new_block);
        BOOST_CHECK(new_block);
    }

    auto chain{chainman->GetChain()};
    auto entry_0{chain.GetByHeight(0)};
    auto entry_1{chain.GetByHeight(1)};
    auto entry_2{chain.GetByHeight(2)};

    // Test inequality
    BOOST_CHECK(entry_0 != entry_1);
    BOOST_CHECK(entry_1 != entry_2);
    BOOST_CHECK(entry_0 != entry_2);

    // Test equality with same entry
    BOOST_CHECK(entry_0 == chain.GetByHeight(0));
    BOOST_CHECK(entry_0 == BlockTreeEntry{entry_0});
    BOOST_CHECK(entry_1 == entry_1);

    // Test GetPrevious
    auto prev{entry_1.GetPrevious()};
    BOOST_CHECK(prev.has_value());
    BOOST_CHECK(prev.value() == entry_0);

    // Test GetAncestor
    BOOST_CHECK(entry_2.GetAncestor(2) == entry_2);
    BOOST_CHECK(entry_2.GetAncestor(1) == entry_1);
    BOOST_CHECK(entry_2.GetAncestor(0) == entry_0);
}

BOOST_AUTO_TEST_CASE(cck_chainman_in_memory_tests)
{
    auto in_memory_test_directory{TestDirectory{"in-memory_test_connectcoin_kernel"}};

    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::REGTEST)};
    auto chainman{create_chainman(
        in_memory_test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/true, /*chainstate_db_in_memory=*/true, context)};

    for (auto& raw_block : REGTEST_BLOCK_DATA) {
        Block block{hex_string_to_byte_vec(raw_block)};
        bool new_block{false};
        chainman->ProcessBlock(block, &new_block);
        BOOST_CHECK(new_block);
    }

    BOOST_CHECK(fs::exists(in_memory_test_directory.m_directory / "blocks"));
    BOOST_CHECK(!fs::exists(in_memory_test_directory.m_directory / "blocks" / "index"));
    BOOST_CHECK(!fs::exists(in_memory_test_directory.m_directory / "chainstate"));

    BOOST_CHECK(context.interrupt());
}

BOOST_AUTO_TEST_CASE(cck_chainman_regtest_tests)
{
    auto test_directory{TestDirectory{"regtest_test_connectcoin_kernel"}};

    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::REGTEST)};

    {
        auto chainman{create_chainman(
            test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
            /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};
        for (const auto& data : REGTEST_BLOCK_DATA) {
            Block block{hex_string_to_byte_vec(data)};
            BlockHeader header = block.GetHeader();
            BlockValidationState state = chainman->ProcessBlockHeader(header);
            BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);
            BOOST_CHECK(state.GetBlockValidationResult() == BlockValidationResult::UNSET);
            BlockTreeEntry entry{*chainman->GetBlockTreeEntry(header.Hash())};
            BOOST_CHECK(!chainman->GetChain().Contains(entry));
            BlockTreeEntry best_entry{chainman->GetBestEntry()};
            BlockHash hash{entry.GetHash()};
            BOOST_CHECK(hash == best_entry.GetHeader().Hash());
        }
    }

    // Validate 206 regtest blocks in total.
    // Stop halfway to check that it is possible to continue validating starting
    // from prior state.
    const size_t mid{REGTEST_BLOCK_DATA.size() / 2};

    {
        auto chainman{create_chainman(
            test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
            /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};
        for (size_t i{0}; i < mid; i++) {
            Block block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[i])};
            bool new_block{false};
            BOOST_CHECK(chainman->ProcessBlock(block, &new_block));
            BOOST_CHECK(new_block);
        }
    }

    auto chainman{create_chainman(
        test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/false, /*chainstate_db_in_memory=*/false, context)};

    for (size_t i{mid}; i < REGTEST_BLOCK_DATA.size(); i++) {
        Block block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[i])};
        bool new_block{false};
        BOOST_CHECK(chainman->ProcessBlock(block, &new_block));
        BOOST_CHECK(new_block);
    }

    auto chain = chainman->GetChain();
    auto tip = chain.Entries().back();
    auto read_block = chainman->ReadBlock(tip).value();
    check_equal(read_block.ToBytes(), hex_string_to_byte_vec(REGTEST_BLOCK_DATA[REGTEST_BLOCK_DATA.size() - 1]));

    auto tip_2 = tip.GetPrevious().value();
    auto read_block_2 = chainman->ReadBlock(tip_2).value();
    check_equal(read_block_2.ToBytes(), hex_string_to_byte_vec(REGTEST_BLOCK_DATA[REGTEST_BLOCK_DATA.size() - 2]));

    Txid txid = read_block.Transactions()[0].Txid();
    Txid txid_2 = read_block_2.Transactions()[0].Txid();
    BOOST_CHECK(txid != txid_2);
    BOOST_CHECK(txid == txid);
    CheckHandle(txid, txid_2);

    auto find_transaction = [&chainman](const TxidView& target_txid) -> std::optional<Transaction> {
        auto chain = chainman->GetChain();
        for (const auto block_tree_entry : chain.Entries()) {
            auto block{chainman->ReadBlock(block_tree_entry)};
            for (const TransactionView transaction : block->Transactions()) {
                if (transaction.Txid() == target_txid) {
                    return Transaction{transaction};
                }
            }
        }
        return std::nullopt;
    };

    for (const auto block_tree_entry : chain.Entries()) {
        auto block{chainman->ReadBlock(block_tree_entry)};
        for (const auto transaction : block->Transactions()) {
            std::vector<TransactionInput> inputs;
            std::vector<TransactionOutput> spent_outputs;
            for (const auto input : transaction.Inputs()) {
                OutPointView point = input.OutPoint();
                if (point.index() == std::numeric_limits<uint32_t>::max()) {
                    continue;
                }
                inputs.emplace_back(input);
                BOOST_CHECK(point.Txid() != transaction.Txid());
                std::optional<Transaction> tx = find_transaction(point.Txid());
                BOOST_CHECK(tx.has_value());
                BOOST_CHECK(point.Txid() == tx->Txid());
                spent_outputs.emplace_back(tx->GetOutput(point.index()));
            }
            BOOST_CHECK(inputs.size() == spent_outputs.size());
            ScriptVerifyStatus status = ScriptVerifyStatus::OK;
            const PrecomputedTransactionData precomputed_txdata{transaction, spent_outputs};
            for (size_t i{0}; i < inputs.size(); ++i) {
                BOOST_CHECK(spent_outputs[i].GetScriptPubkey().Verify(spent_outputs[i].Amount(), transaction, &precomputed_txdata, i, ScriptVerificationFlags::ALL, status));
            }
        }
    }

    // Read spent outputs for the current tip. The regenerated fixture contains
    // one non-coinbase transaction in its final block.
    BlockSpentOutputs block_spent_outputs{chainman->ReadBlockSpentOutputs(tip)};
    BlockSpentOutputs block_spent_outputs_copy{chainman->ReadBlockSpentOutputs(tip)};
    CheckHandle(block_spent_outputs, block_spent_outputs_copy);
    CheckRange(block_spent_outputs.TxsSpentOutputs(), block_spent_outputs.Count());
    BOOST_CHECK_EQUAL(block_spent_outputs.Count(), 1);

    // Get transaction spent outputs from the final block.
    TransactionSpentOutputsView transaction_spent_outputs{block_spent_outputs.GetTxSpentOutputs(block_spent_outputs.Count() - 1)};
    TransactionSpentOutputs owned_transaction_spent_outputs{transaction_spent_outputs};
    TransactionSpentOutputs owned_transaction_spent_outputs_copy{transaction_spent_outputs};
    CheckHandle(owned_transaction_spent_outputs, owned_transaction_spent_outputs_copy);
    CheckRange(transaction_spent_outputs.Coins(), transaction_spent_outputs.Count());

    // Get the last coin from the transaction spent outputs
    CoinView coin{transaction_spent_outputs.GetCoin(transaction_spent_outputs.Count() - 1)};
    BOOST_CHECK(coin.IsCoinbase());
    Coin owned_coin{coin};
    Coin owned_coin_copy{coin};
    CheckHandle(owned_coin, owned_coin_copy);

    // Validate coin properties
    TransactionOutputView output = coin.GetOutput();
    uint32_t coin_height = coin.GetConfirmationHeight();
    BOOST_CHECK_EQUAL(coin_height, 58);
    BOOST_CHECK_EQUAL(output.Amount(), 1'000'000'000'000);

    // Test script pubkey serialization
    auto script_pubkey = output.GetScriptPubkey();
    auto script_pubkey_bytes{script_pubkey.ToBytes()};
    BOOST_CHECK_EQUAL(script_pubkey_bytes.size(), 34);
    auto round_trip_script_pubkey{ScriptPubkey(script_pubkey_bytes)};
    BOOST_CHECK_EQUAL(round_trip_script_pubkey.ToBytes().size(), 34);

    for (const auto tx_spent_outputs : block_spent_outputs.TxsSpentOutputs()) {
        for (const auto coins : tx_spent_outputs.Coins()) {
            BOOST_CHECK_GT(coins.GetOutput().Amount(), 1);
        }
    }

    CheckRange(chain.Entries(), chain.CountEntries());

    for (const BlockTreeEntry entry : chain.Entries()) {
        std::optional<Block> block{chainman->ReadBlock(entry)};
        if (block) {
            for (const TransactionView transaction : block->Transactions()) {
                for (const TransactionOutputView output : transaction.Outputs()) {
                    // skip data carrier outputs
                    if ((unsigned char)output.GetScriptPubkey().ToBytes()[0] == 0x6a) {
                        continue;
                    }
                    BOOST_CHECK_GT(output.Amount(), 1);
                }
            }
        }
    }

    int32_t count{0};
    for (const auto entry : chain.Entries()) {
        BOOST_CHECK_EQUAL(entry.GetHeight(), count);
        ++count;
    }
    BOOST_CHECK_EQUAL(count, chain.CountEntries());


    fs::remove(test_directory.m_directory / "blocks" / "blk00000.dat");
    BOOST_CHECK(!chainman->ReadBlock(tip_2).has_value());
    fs::remove(test_directory.m_directory / "blocks" / "rev00000.dat");
    BOOST_CHECK_THROW(chainman->ReadBlockSpentOutputs(tip), std::runtime_error);
}

// -----------------------------------------------------------------------------
// CheckTransaction tests
//
// Transaction hex below is copied from src/test/data/tx_invalid.json (entries
// marked "BADTX") and tx_valid.json. CheckTransaction performs only basic context-free
// consensus checks and can only produce two outcomes:
//   - VALID  (ValidationMode::VALID, TxValidationResult::UNSET)
//   - INVALID (ValidationMode::INVALID, TxValidationResult::CONSENSUS)
// Other TxValidationResult values are set by higher-level validation and are
// not reachable through cck_transaction_check.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(cck_transaction_check_tests)
{
    using namespace cck;

    const std::string non_null_hash{"01" + std::string(62, '0')};
    const std::string regular_input{non_null_hash + "0000000000ffffffff"};
    const std::string null_input{std::string(64, '0') + "ffffffff00ffffffff"};
    const std::string typed_output_zero{"000000000000000001" + std::string{TEST_XONLY_PUBKEY}};
    const std::string valid_tx_hex{TEST_SPENDING_TX};
    const std::string no_outputs_tx_hex{"0100000001" + regular_input + "0000000000"};

    auto expect_valid = [](std::string_view hex) {
        Transaction tx{hex_string_to_byte_vec(hex)};
        TxValidationState st;
        BOOST_CHECK(CheckTransaction(tx, st));
        BOOST_CHECK(st.GetValidationMode() == ValidationMode::VALID);
        BOOST_CHECK(st.GetTxValidationResult() == TxValidationResult::UNSET);
    };

    auto expect_invalid = [](std::string_view hex) {
        Transaction tx{hex_string_to_byte_vec(hex)};
        TxValidationState st;
        BOOST_CHECK(!CheckTransaction(tx, st));
        BOOST_CHECK(st.GetValidationMode() == ValidationMode::INVALID);
        BOOST_CHECK(st.GetTxValidationResult() == TxValidationResult::CONSENSUS);
    };

    // Valid: a current type-1 P2PK transaction generated on regtest.
    expect_valid(valid_tx_hex);

    // Valid coinbase with scriptSig size 2 and one typed output.
    expect_valid("0100000001" + std::string(64, '0') + "ffffffff025151ffffffff01" + typed_output_zero + "00000000");

    // No outputs (BADTX from tx_invalid.json)
    expect_invalid(no_outputs_tx_hex);

    {
        Transaction valid_tx{hex_string_to_byte_vec(valid_tx_hex)};
        Transaction invalid_tx{hex_string_to_byte_vec(no_outputs_tx_hex)};
        TxValidationState state;

        BOOST_CHECK(cck_transaction_check(valid_tx.get(), state.get()) == 1);
        BOOST_CHECK(state.GetValidationMode() == ValidationMode::VALID);
        BOOST_CHECK(state.GetTxValidationResult() == TxValidationResult::UNSET);

        BOOST_CHECK(cck_transaction_check(invalid_tx.get(), state.get()) == 0);
        BOOST_CHECK(state.GetValidationMode() == ValidationMode::INVALID);
        BOOST_CHECK(state.GetTxValidationResult() == TxValidationResult::CONSENSUS);
    }

    // Negative output, MAX_MONEY + 1, and aggregate overflow.
    expect_invalid("0100000001" + regular_input + "01ffffffffffffffff01" + std::string{TEST_XONLY_PUBKEY} + "00000000");
    const std::string max_output{"000064a7b3b6e00d01" + std::string{TEST_XONLY_PUBKEY}};
    const std::string max_plus_one_output{"010064a7b3b6e00d01" + std::string{TEST_XONLY_PUBKEY}};
    const std::string one_output{"010000000000000001" + std::string{TEST_XONLY_PUBKEY}};
    expect_invalid("0100000001" + regular_input + "01" + max_plus_one_output + "00000000");
    expect_invalid("0100000001" + regular_input + "02" + max_output + one_output + "00000000");

    // Unknown/reserved output type and duplicate inputs.
    expect_invalid("0100000001" + regular_input + "01000000000000000000" + "00000000");
    expect_invalid("0100000002" + regular_input + regular_input + "01" + typed_output_zero + "00000000");

    // Coinbase scripts outside the allowed 2..100 byte range.
    expect_invalid("0100000001" + std::string(64, '0') + "ffffffff0151ffffffff01" + typed_output_zero + "00000000");
    expect_invalid("0100000001" + std::string(64, '0') + "ffffffff65" + std::string(202, '1') + "ffffffff01" + typed_output_zero + "00000000");

    // Null prevout in a non-coinbase transaction.
    expect_invalid("0100000002" + null_input + regular_input + "01" + typed_output_zero + "00000000");
}

class KernelMockTime
{
public:
    explicit KernelMockTime(std::chrono::seconds timestamp) { set(timestamp); }
    ~KernelMockTime()
    {
        set_mock_time(std::chrono::seconds{0});
    }

    KernelMockTime(const KernelMockTime&) = delete;
    KernelMockTime& operator=(const KernelMockTime&) = delete;

    void set(std::chrono::seconds timestamp) { set_mock_time(timestamp); }
};

BOOST_AUTO_TEST_CASE(cck_set_mock_time_tests)
{
    // Out-of-range timestamps throw
    BOOST_CHECK_EXCEPTION(set_mock_time(std::chrono::seconds{-1}), std::runtime_error, HasReason("timestamp out of range"));
    constexpr std::chrono::seconds max_time{std::numeric_limits<uint32_t>::max()};
    BOOST_CHECK_EXCEPTION(set_mock_time(max_time + std::chrono::seconds{1}), std::runtime_error, HasReason("timestamp out of range"));

    // Confirm the mock time actually takes effect by exercising the header future-time check
    auto test_directory{TestDirectory{"set_mock_time_test_connectcoin_kernel"}};
    auto notifications{std::make_shared<TestKernelNotifications>()};
    auto context{create_context(notifications, ChainType::REGTEST)};
    auto chainman{create_chainman(
        test_directory, /*reindex=*/false, /*wipe_chainstate=*/false,
        /*block_tree_db_in_memory=*/true, /*chainstate_db_in_memory=*/true, context)};

    Block block{hex_string_to_byte_vec(REGTEST_BLOCK_DATA[0])};
    BlockHeader header{block.GetHeader()};
    const std::chrono::seconds block_time{header.Timestamp()};

    // With the time set 3h before the header, the kernel must see the header as >2h in the future and reject it
    KernelMockTime mock_time{block_time - std::chrono::hours{3}};
    BlockValidationState future_state{chainman->ProcessBlockHeader(header)};
    BOOST_CHECK(future_state.GetValidationMode() == ValidationMode::INVALID);
    BOOST_CHECK(future_state.GetBlockValidationResult() == BlockValidationResult::TIME_FUTURE);

    // At the upper bound the header is far in the past and must be accepted; this also
    // confirms the future-time check's "now + 2h" computation doesn't overflow when now is at its max.
    mock_time.set(max_time);
    BlockValidationState ok_state{chainman->ProcessBlockHeader(header)};
    BOOST_CHECK(ok_state.GetValidationMode() == ValidationMode::VALID);
    BOOST_CHECK(ok_state.GetBlockValidationResult() == BlockValidationResult::UNSET);
}
