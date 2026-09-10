// Copyright (c) 2026 The ConnectCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/history.h>

#include <chainparams.h>
#include <hash.h>
#include <streams.h>
#include <uint256.h>
#include <util/fs_helpers.h>
#include <util/translation.h>
#include <wallet/db.h>
#include <wallet/sqlite.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallet {
namespace {

const auto& HistoryTypes()
{
    // DBKeys strings live in another translation unit. Initialize lazily to
    // avoid depending on cross-translation-unit static initialization order.
    static const std::array types{DBKeys::TX, DBKeys::WTX_VARIANT, DBKeys::LOCKED_UTXO};
    return types;
}

struct RecordsDigest {
    uint256 all;
    uint256 preserved;
    size_t count{0};
    size_t history_count{0};
    friend bool operator==(const RecordsDigest&, const RecordsDigest&) = default;
};

RecordsDigest DigestRecords(DatabaseBatch& batch)
{
    auto cursor = batch.GetNewCursor();
    if (!cursor) throw std::runtime_error("Unable to create wallet verification cursor");
    std::vector<uint256> all, preserved;
    size_t history_count{0};
    while (true) {
        DataStream key, value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status != DatabaseCursor::Status::MORE) throw std::runtime_error("Unable to read wallet verification record");
        // Hash lengths as well as contents. Sorting hashes makes this independent
        // of the backend's cursor order, without retaining copies of private data.
        const auto digest = (HashWriter{} << std::span{key} << std::span{value}).GetHash();
        all.push_back(digest);
        std::string type;
        key >> type; // A malformed record key must fail before any deletion.
        const auto& types = HistoryTypes();
        if (std::find(types.begin(), types.end(), type) != types.end()) {
            ++history_count;
        } else {
            preserved.push_back(digest);
        }
    }
    std::sort(all.begin(), all.end());
    std::sort(preserved.begin(), preserved.end());
    return {(HashWriter{} << all).GetHash(), (HashWriter{} << preserved).GetHash(), all.size(), history_count};
}

} // namespace

bool ResetTransactionHistory(WalletDatabase& database, const fs::path& backup_dir,
                             size_t& removed_records, bilingual_str& error)
{
    removed_records = 0;
    error = {};
    std::unique_ptr<DatabaseBatch> batch;
    try {
        if (Params().GetChainType() == ChainType::MAIN) {
            throw std::runtime_error("reset-tx-history is restricted to test networks");
        }
        auto* sqlite = dynamic_cast<SQLiteDatabase*>(&database);
        if (!sqlite || !sqlite->Verify(error)) {
            if (error.empty()) error = Untranslated("reset-tx-history requires a verified SQLite test-network wallet");
            return false;
        }
        // Verify() checks the actual application_id against the selected chain
        // (including a custom signet challenge), not just a CLI network flag.
        if (sqlite->HasActiveTxn()) throw std::runtime_error("Wallet already has an active database transaction");
        batch = database.MakeBatch();
        const auto before = DigestRecords(*batch);
        if (backup_dir.empty() || !backup_dir.is_absolute()) {
            throw std::runtime_error("An absolute, new backup directory is required");
        }
        // Creating a directory is exclusive: never overwrite a previous backup,
        // including a dangling symlink. No source records have changed yet.
        if (!fs::create_directory(backup_dir)) throw std::runtime_error("Backup directory already exists; choose a new directory");
        fs::permissions(backup_dir, fs::perms::owner_all, fs::perm_options::replace);
        const fs::path backup_file{backup_dir / "wallet.dat"};
        if (!database.Backup(fs::PathToString(backup_file))) throw std::runtime_error("Unable to create complete wallet backup; source history was not changed");

        DatabaseOptions options;
        options.require_existing = true;
        options.require_format = DatabaseFormat::SQLITE;
        DatabaseStatus status;
        auto backup = MakeDatabase(backup_dir, options, status, error);
        if (!backup) return false;
        auto backup_batch = backup->MakeBatch();
        if (DigestRecords(*backup_batch) != before) throw std::runtime_error("Wallet backup record verification failed; source history was not changed");
        backup_batch.reset();
        backup.reset();

        // Verification may have read cached pages. Flush the verified file and
        // both directory entries before committing any destructive source write.
        AutoFile backup_handle{fsbridge::fopen(backup_file, "rb+")};
        if (backup_handle.IsNull() || !backup_handle.Commit()) throw std::runtime_error("Unable to sync verified wallet backup; source history was not changed");
        if (backup_handle.fclose() != 0) throw std::runtime_error("Unable to close verified wallet backup; source history was not changed");
        for (const auto& dir : std::array<fs::path, 2>{backup_dir, backup_dir.parent_path()}) {
            const auto synced = DirectoryCommitChecked(dir);
            if (synced.has_value() && !*synced) throw std::runtime_error("Unable to sync wallet backup directory; source history was not changed");
            // Windows has no directory-sync implementation. Do not claim this
            // establishes power-loss durability there; see the tool's warning.
        }

        if (!batch->TxnBegin()) throw std::runtime_error("Unable to begin wallet history reset transaction");
        if (DigestRecords(*batch) != before) throw std::runtime_error("Wallet changed after backup; refusing history reset");
        for (const auto& type : HistoryTypes()) {
            if (!batch->ErasePrefix(DataStream{} << type)) throw std::runtime_error("Unable to erase wallet history records");
        }
        const auto after = DigestRecords(*batch);
        if (after.history_count != 0 || after.count != before.count - before.history_count || after.preserved != before.preserved) {
            throw std::runtime_error("Wallet preservation check failed; refusing to commit history reset");
        }
        if (!batch->TxnCommit()) throw std::runtime_error("Unable to commit wallet history reset");
        removed_records = before.history_count;
        return true;
    } catch (const std::exception& exception) {
        error = Untranslated(exception.what());
        if (batch && batch->HasActiveTxn() && !batch->TxnAbort()) {
            error += Untranslated(". Database rollback reported an error; stop and use the verified backup before further recovery.");
        }
        return false;
    }
}

} // namespace wallet
