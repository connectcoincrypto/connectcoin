// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <crypto/common.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/translation.h>
#include <wallet/sqlite.h>
#include <wallet/migrate.h>
#include <wallet/test/util.h>
#include <wallet/walletutil.h>

#include <sqlite3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline std::ostream& operator<<(std::ostream& os, const std::pair<const SerializeData, SerializeData>& kv)
{
    std::span key{kv.first}, value{kv.second};
    os << "(\"" << std::string_view{reinterpret_cast<const char*>(key.data()), key.size()} << "\", \""
       << std::string_view{reinterpret_cast<const char*>(value.data()), value.size()} << "\")";
    return os;
}

namespace wallet {

inline std::span<const std::byte> StringBytes(std::string_view str)
{
    return std::as_bytes(std::span{str});
}

static SerializeData StringData(std::string_view str)
{
    auto bytes = StringBytes(str);
    return SerializeData{bytes.begin(), bytes.end()};
}

static void CheckPrefix(DatabaseBatch& batch, std::span<const std::byte> prefix, MockableData expected)
{
    std::unique_ptr<DatabaseCursor> cursor = batch.GetNewPrefixCursor(prefix);
    MockableData actual;
    while (true) {
        DataStream key, value;
        DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        BOOST_CHECK(status == DatabaseCursor::Status::MORE);
        BOOST_CHECK(
            actual.emplace(SerializeData(key.begin(), key.end()), SerializeData(value.begin(), value.end())).second);
    }
    BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

BOOST_FIXTURE_TEST_SUITE(db_tests, BasicTestingSetup)

static std::vector<std::unique_ptr<const CChainParams>> WalletIdentityNetworks()
{
    std::vector<std::unique_ptr<const CChainParams>> networks;
    networks.emplace_back(CChainParams::Main());
    networks.emplace_back(CChainParams::TestNet());
    networks.emplace_back(CChainParams::TestNet4());
    networks.emplace_back(CChainParams::SigNet());
    networks.emplace_back(CChainParams::RegTest());
    for (const uint8_t challenge : std::array<uint8_t, 2>{0x52, 0x53}) {
        CChainParams::SigNetOptions options;
        options.challenge = std::vector<uint8_t>{challenge};
        networks.emplace_back(CChainParams::SigNet(options));
    }
    return networks;
}

class WalletWireMagicTestParams : public CChainParams
{
public:
    WalletWireMagicTestParams(const CChainParams& params, const MessageStartChars& wire_magic) : CChainParams{params}
    {
        pchMessageStart = wire_magic;
    }
};

BOOST_AUTO_TEST_CASE(wallet_database_ids_are_stable)
{
    // These are the wallet IDs used before the P2C v2 network reset, not the
    // new wire magics. Signet IDs are SHA256d(0151), SHA256d(0152), SHA256d(0153).
    const std::array<MessageStartChars, 7> expected{{
        {0xd9, 0x51, 0xa5, 0xe2},
        {0x03, 0x84, 0x8e, 0x59},
        {0xbb, 0x51, 0xf5, 0xe7},
        {0x54, 0xd2, 0x6f, 0xbd},
        {0xa5, 0x4f, 0xc7, 0xd5},
        {0x2c, 0x79, 0x81, 0x94},
        {0x31, 0x4b, 0xfb, 0x9c},
    }};
    const auto networks = WalletIdentityNetworks();
    BOOST_REQUIRE_EQUAL(networks.size(), expected.size());
    std::set<MessageStartChars> distinct;
    for (size_t i = 0; i < networks.size(); ++i) {
        BOOST_CHECK(networks[i]->WalletDatabaseId() == expected[i]);
        BOOST_CHECK(distinct.insert(networks[i]->WalletDatabaseId()).second);
        if (networks[i]->GetChainType() != ChainType::MAIN) {
            BOOST_CHECK(networks[i]->WalletDatabaseId() != networks[i]->MessageStart());
        }
        const WalletWireMagicTestParams changed_wire{*networks[i], {0x12, 0x34, 0x56, 0x78}};
        BOOST_CHECK(changed_wire.WalletDatabaseId() == expected[i]);
    }
}

BOOST_AUTO_TEST_CASE(sqlite_wallet_identity_isolated_from_wire_magic)
{
    // Preserve even custom/test-only parameters if an assertion throws.
    struct RestoreParams {
        std::unique_ptr<const CChainParams> value{std::make_unique<const CChainParams>(Params())};
        ~RestoreParams() { SelectParams(std::move(value)); }
    } restore_params;
    const auto networks = WalletIdentityNetworks();
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;

    for (size_t i = 0; i < networks.size(); ++i) {
        // CChainParams has no virtual destructor: store a sliced base value.
        SelectParams(std::make_unique<const CChainParams>(WalletWireMagicTestParams{*networks[i], {0x12, 0x34, 0x56, 0x78}}));
        const fs::path path{m_path_root / fs::PathFromString(strprintf("wallet-id-%u", i))};
        error = {};
        auto database = MakeSQLiteDatabase(path, options, status, error);
        BOOST_REQUIRE_MESSAGE(database != nullptr, error.original);
        const fs::path file{fs::PathFromString(database->Filename())};
        BOOST_CHECK(IsSQLiteFile(file));
        {
            // Inspect the actual new file, independently of the SQLite verifier.
            std::ifstream input{file.std_path(), std::ios::binary};
            MessageStartChars stored{};
            input.seekg(68);
            input.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
            BOOST_REQUIRE(input.good());
            BOOST_CHECK(stored == networks[i]->WalletDatabaseId());
        }
        for (size_t j = 0; j < networks.size(); ++j) {
            // A different chain/challenge must remain rejected even if its
            // P2P magic happens to equal this file's application ID.
            SelectParams(std::make_unique<const CChainParams>(WalletWireMagicTestParams{*networks[j], networks[i]->WalletDatabaseId()}));
            BOOST_CHECK_EQUAL(IsSQLiteFile(file), i == j);
            error = {};
            BOOST_CHECK_EQUAL(database->Verify(error), i == j);
            if (i != j) BOOST_CHECK(error.original.find("Unexpected application id") != std::string::npos);
        }
        database.reset();

        // Test the normal open+Verify path, not just Verify on an existing handle.
        for (size_t j = 0; j < networks.size(); ++j) {
            SelectParams(std::make_unique<const CChainParams>(*networks[j]));
            error = {};
            auto reopened = MakeSQLiteDatabase(path, options, status, error);
            BOOST_CHECK_EQUAL(bool(reopened), i == j);
            BOOST_CHECK(status == (i == j ? DatabaseStatus::SUCCESS : DatabaseStatus::FAILED_VERIFY));
        }

        SelectParams(std::make_unique<const CChainParams>(*networks[i]));
        error = {};
        database = MakeSQLiteDatabase(path, options, status, error);
        BOOST_REQUIRE_MESSAGE(database != nullptr, error.original);
        if (Params().MessageStart() != Params().WalletDatabaseId()) {
            // Unreleased intermediate v2 test wallets are not a compatibility
            // alias. In particular never accept P2P magic as a fallback ID.
            const uint32_t transient_id = ReadBE32(Params().MessageStart().data());
            SQliteExecHandler sql;
            BOOST_REQUIRE_EQUAL(sql.Exec(*database, strprintf("PRAGMA application_id=%d", static_cast<int32_t>(transient_id))), SQLITE_OK);
            BOOST_CHECK(!IsSQLiteFile(file));
            error = {};
            BOOST_CHECK(!database->Verify(error));
            BOOST_CHECK(error.original.find("Unexpected application id") != std::string::npos);
        }
    }
}

static std::vector<std::unique_ptr<WalletDatabase>> TestDatabases(const fs::path& path_root)
{
    std::vector<std::unique_ptr<WalletDatabase>> dbs;
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    // Unable to test BerkeleyRO since we cannot create a new BDB database to open
    dbs.emplace_back(MakeSQLiteDatabase(path_root / "sqlite", options, status, error));
    dbs.emplace_back(CreateMockableWalletDatabase());
    return dbs;
}

BOOST_AUTO_TEST_CASE(db_cursor_prefix_range_test)
{
    // Test each supported db
    for (const auto& database : TestDatabases(m_path_root)) {
        std::vector<std::string> prefixes = {"", "FIRST", "SECOND", "P\xfe\xff", "P\xff\x01", "\xff\xff"};

        std::unique_ptr<DatabaseBatch> handler = Assert(database)->MakeBatch();
        // Write elements to it
        for (unsigned int i = 0; i < 10; i++) {
            for (const auto& prefix : prefixes) {
                BOOST_CHECK(handler->Write(std::make_pair(prefix, i), i));
            }
        }

        // Now read all the items by prefix and verify that each element gets parsed correctly
        for (const auto& prefix : prefixes) {
            DataStream s_prefix;
            s_prefix << prefix;
            std::unique_ptr<DatabaseCursor> cursor = handler->GetNewPrefixCursor(s_prefix);
            DataStream key;
            DataStream value;
            for (int i = 0; i < 10; i++) {
                DatabaseCursor::Status status = cursor->Next(key, value);
                BOOST_CHECK_EQUAL(status, DatabaseCursor::Status::MORE);

                std::string key_back;
                unsigned int i_back;
                key >> key_back >> i_back;
                BOOST_CHECK_EQUAL(key_back, prefix);

                unsigned int value_back;
                value >> value_back;
                BOOST_CHECK_EQUAL(value_back, i_back);
            }

            // Let's now read it once more, it should return DONE
            BOOST_CHECK(cursor->Next(key, value) == DatabaseCursor::Status::DONE);
        }
        handler.reset();
        database->Close();
    }
}

// Lower level DatabaseBase::GetNewPrefixCursor test, to cover cases that aren't
// covered in the higher level test above. The higher level test uses
// serialized strings which are prefixed with string length, so it doesn't test
// truly empty prefixes or prefixes that begin with \xff
BOOST_AUTO_TEST_CASE(db_cursor_prefix_byte_test)
{
    const MockableData::value_type
        e{StringData(""), StringData("e")},
        p{StringData("prefix"), StringData("p")},
        ps{StringData("prefixsuffix"), StringData("ps")},
        f{StringData("\xff"), StringData("f")},
        fs{StringData("\xffsuffix"), StringData("fs")},
        ff{StringData("\xff\xff"), StringData("ff")},
        ffs{StringData("\xff\xffsuffix"), StringData("ffs")};
    for (const auto& database : TestDatabases(m_path_root)) {
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();

        // Write elements to it if not berkeleyro
        for (const auto& [k, v] : {e, p, ps, f, fs, ff, ffs}) {
            batch->Write(std::span{k}, std::span{v});
        }

        CheckPrefix(*batch, StringBytes(""), {e, p, ps, f, fs, ff, ffs});
        CheckPrefix(*batch, StringBytes("prefix"), {p, ps});
        CheckPrefix(*batch, StringBytes("\xff"), {f, fs, ff, ffs});
        CheckPrefix(*batch, StringBytes("\xff\xff"), {ff, ffs});
        batch.reset();
        database->Close();
    }
}

BOOST_AUTO_TEST_CASE(db_availability_after_write_error)
{
    // Ensures the database remains accessible without deadlocking after a write error.
    // To simulate the behavior, record overwrites are disallowed, and the test verifies
    // that the database remains active after failing to store an existing record.
    for (const auto& database : TestDatabases(m_path_root)) {
        // Write original record
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        std::string key = "key";
        std::string value = "value";
        std::string value2 = "value_2";
        BOOST_CHECK(batch->Write(key, value));
        // Attempt to overwrite the record (expect failure)
        BOOST_CHECK(!batch->Write(key, value2, /*fOverwrite=*/false));
        // Successfully overwrite the record
        BOOST_CHECK(batch->Write(key, value2, /*fOverwrite=*/true));
        // Sanity-check; read and verify the overwritten value
        std::string read_value;
        BOOST_CHECK(batch->Read(key, read_value));
        BOOST_CHECK_EQUAL(read_value, value2);
    }
}

// Verify 'ErasePrefix' functionality using db keys similar to the ones used by the wallet.
// Keys are in the form of std::pair<TYPE, ENTRY_ID>
BOOST_AUTO_TEST_CASE(erase_prefix)
{
    const std::string key = "key";
    const std::string key2 = "key2";
    const std::string value = "value";
    const std::string value2 = "value_2";
    auto make_key = [](std::string type, std::string id) { return std::make_pair(type, id); };

    for (const auto& database : TestDatabases(m_path_root)) {
        if (dynamic_cast<BerkeleyRODatabase*>(database.get())) {
            // Skip this test if BerkeleyRO
            continue;
        }
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();

        // Write two entries with the same key type prefix, a third one with a different prefix
        // and a fourth one with the type-id values inverted
        BOOST_CHECK(batch->Write(make_key(key, value), value));
        BOOST_CHECK(batch->Write(make_key(key, value2), value2));
        BOOST_CHECK(batch->Write(make_key(key2, value), value));
        BOOST_CHECK(batch->Write(make_key(value, key), value));

        // Erase the ones with the same prefix and verify result
        BOOST_CHECK(batch->TxnBegin());
        BOOST_CHECK(batch->ErasePrefix(DataStream() << key));
        BOOST_CHECK(batch->TxnCommit());

        BOOST_CHECK(!batch->Exists(make_key(key, value)));
        BOOST_CHECK(!batch->Exists(make_key(key, value2)));
        // Also verify that entries with a different prefix were not erased
        BOOST_CHECK(batch->Exists(make_key(key2, value)));
        BOOST_CHECK(batch->Exists(make_key(value, key)));
    }
}

// Test-only statement execution error
constexpr int TEST_SQLITE_ERROR = -999;

class DbExecBlocker : public SQliteExecHandler
{
private:
    SQliteExecHandler m_base_exec;
    std::set<std::string> m_blocked_statements;
public:
    DbExecBlocker(std::set<std::string> blocked_statements) : m_blocked_statements(blocked_statements) {}
    int Exec(SQLiteDatabase& database, const std::string& statement) override {
        if (m_blocked_statements.contains(statement)) return TEST_SQLITE_ERROR;
        return m_base_exec.Exec(database, statement);
    }
};

BOOST_AUTO_TEST_CASE(txn_close_failure_dangling_txn)
{
    // Verifies that there is no active dangling, to-be-reversed db txn
    // after the batch object that initiated it is destroyed.
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    std::unique_ptr<SQLiteDatabase> database = MakeSQLiteDatabase(m_path_root / "sqlite", options, status, error);

    std::string key = "key";
    std::string value = "value";

    std::unique_ptr<SQLiteBatch> batch = std::make_unique<SQLiteBatch>(*database);
    BOOST_CHECK(batch->TxnBegin());
    BOOST_CHECK(batch->Write(key, value));
    // Set a handler to prevent txn abortion during destruction.
    // Mimicking a db statement execution failure.
    batch->SetExecHandler(std::make_unique<DbExecBlocker>(std::set<std::string>{"ROLLBACK TRANSACTION"}));
    // Destroy batch
    batch.reset();

    // Ensure there is no dangling, to-be-reversed db txn
    BOOST_CHECK(!database->HasActiveTxn());

    // And, just as a sanity check; verify that new batchs only write what they suppose to write
    // and nothing else.
    std::string key2 = "key2";
    std::unique_ptr<SQLiteBatch> batch2 = std::make_unique<SQLiteBatch>(*database);
    BOOST_CHECK(batch2->Write(key2, value));
    // The first key must not exist
    BOOST_CHECK(!batch2->Exists(key));
}

BOOST_AUTO_TEST_CASE(concurrent_txn_dont_interfere)
{
    std::string key = "key";
    std::string value = "value";
    std::string value2 = "value_2";

    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    const auto& database = MakeSQLiteDatabase(m_path_root / "sqlite", options, status, error);

    std::unique_ptr<DatabaseBatch> handler = Assert(database)->MakeBatch();

    // Verify concurrent db transactions does not interfere between each other.
    // Start db txn, write key and check the key does exist within the db txn.
    BOOST_CHECK(handler->TxnBegin());
    BOOST_CHECK(handler->Write(key, value));
    BOOST_CHECK(handler->Exists(key));

    // But, the same key, does not exist in another handler
    std::unique_ptr<DatabaseBatch> handler2 = Assert(database)->MakeBatch();
    BOOST_CHECK(handler2->Exists(key));

    // Attempt to commit the handler txn calling the handler2 methods.
    // Which, must not be possible.
    BOOST_CHECK(!handler2->TxnCommit());
    BOOST_CHECK(!handler2->TxnAbort());

    // Only the first handler can commit the changes.
    BOOST_CHECK(handler->TxnCommit());
    // And, once commit is completed, handler2 can read the record
    std::string read_value;
    BOOST_CHECK(handler2->Read(key, read_value));
    BOOST_CHECK_EQUAL(read_value, value);

    // Also, once txn is committed, single write statements are re-enabled.
    // Which means that handler2 can read the record changes directly.
    BOOST_CHECK(handler->Write(key, value2, /*fOverwrite=*/true));
    BOOST_CHECK(handler2->Read(key, read_value));
    BOOST_CHECK_EQUAL(read_value, value2);
}

BOOST_AUTO_TEST_CASE(in_memory_database_cannot_reopen)
{
    // Reopening an in-memory database would create a fresh empty connection,
    // silently losing all data. Open() must throw instead.
    InMemoryWalletDatabase database;
    database.Close();
    BOOST_CHECK_THROW(database.Open(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
