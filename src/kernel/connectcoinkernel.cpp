// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define CONNECTCOINKERNEL_BUILD

#include <kernel/connectcoinkernel.h>

#include <chain.h>
#include <coins.h>
#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/chainparams.h>
#include <kernel/checks.h>
#include <kernel/context.h>
#include <kernel/notifications_interface.h>
#include <kernel/warning.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/verify_flags.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/task_runner.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Consensus {
struct Params;
} // namespace Consensus

using kernel::ChainstateRole;
using util::ImmediateTaskRunner;

// Define G_TRANSLATION_FUN symbol in libconnectcoinkernel library so users of the
// library aren't required to export this symbol
extern const TranslateFn G_TRANSLATION_FUN{nullptr};

static const kernel::Context cck_context_static{};

namespace {

bool is_valid_flag_combination(script_verify_flags flags)
{
    if (flags & SCRIPT_VERIFY_CLEANSTACK && ~flags & (SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS)) return false;
    if (flags & SCRIPT_VERIFY_WITNESS && ~flags & SCRIPT_VERIFY_P2SH) return false;
    return true;
}

class WriterStream
{
private:
    cck_WriteBytes m_writer;
    void* m_user_data;

public:
    WriterStream(cck_WriteBytes writer, void* user_data)
        : m_writer{writer}, m_user_data{user_data} {}

    //
    // Stream subset
    //
    void write(std::span<const std::byte> src)
    {
        if (m_writer(src.data(), src.size(), m_user_data) != 0) {
            throw std::runtime_error("Failed to write serialization data");
        }
    }

    template <typename T>
    WriterStream& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return *this;
    }
};

template <typename C, typename CPP>
struct Handle {
    static C* ref(CPP* cpp_type)
    {
        return reinterpret_cast<C*>(cpp_type);
    }

    static const C* ref(const CPP* cpp_type)
    {
        return reinterpret_cast<const C*>(cpp_type);
    }

    template <typename... Args>
    static C* create(Args&&... args)
    {
        auto cpp_obj{std::make_unique<CPP>(std::forward<Args>(args)...)};
        return ref(cpp_obj.release());
    }

    static C* copy(const C* ptr)
    {
        auto cpp_obj{std::make_unique<CPP>(get(ptr))};
        return ref(cpp_obj.release());
    }

    static const CPP& get(const C* ptr)
    {
        return *reinterpret_cast<const CPP*>(ptr);
    }

    static CPP& get(C* ptr)
    {
        return *reinterpret_cast<CPP*>(ptr);
    }

    static void operator delete(void* ptr)
    {
        delete reinterpret_cast<CPP*>(ptr);
    }
};

} // namespace

struct cck_BlockTreeEntry: Handle<cck_BlockTreeEntry, CBlockIndex> {};
struct cck_Block : Handle<cck_Block, std::shared_ptr<const CBlock>> {};
struct cck_BlockValidationState : Handle<cck_BlockValidationState, BlockValidationState> {};
struct cck_TxValidationState : Handle<cck_TxValidationState, TxValidationState> {};

namespace {

BCLog::Level get_bclog_level(cck_LogLevel level)
{
    switch (level) {
    case cck_LogLevel_INFO: {
        return BCLog::Level::Info;
    }
    case cck_LogLevel_DEBUG: {
        return BCLog::Level::Debug;
    }
    case cck_LogLevel_TRACE: {
        return BCLog::Level::Trace;
    }
    }
    assert(false);
}

BCLog::LogFlags get_bclog_flag(cck_LogCategory category)
{
    switch (category) {
    case cck_LogCategory_BENCH: {
        return BCLog::LogFlags::BENCH;
    }
    case cck_LogCategory_BLOCKSTORAGE: {
        return BCLog::LogFlags::BLOCKSTORAGE;
    }
    case cck_LogCategory_COINDB: {
        return BCLog::LogFlags::COINDB;
    }
    case cck_LogCategory_LEVELDB: {
        return BCLog::LogFlags::LEVELDB;
    }
    case cck_LogCategory_MEMPOOL: {
        return BCLog::LogFlags::MEMPOOL;
    }
    case cck_LogCategory_PRUNE: {
        return BCLog::LogFlags::PRUNE;
    }
    case cck_LogCategory_RAND: {
        return BCLog::LogFlags::RAND;
    }
    case cck_LogCategory_REINDEX: {
        return BCLog::LogFlags::REINDEX;
    }
    case cck_LogCategory_VALIDATION: {
        return BCLog::LogFlags::VALIDATION;
    }
    case cck_LogCategory_KERNEL: {
        return BCLog::LogFlags::KERNEL;
    }
    case cck_LogCategory_ALL: {
        return BCLog::LogFlags::ALL;
    }
    }
    assert(false);
}

cck_SynchronizationState cast_state(SynchronizationState state)
{
    switch (state) {
    case SynchronizationState::INIT_REINDEX:
        return cck_SynchronizationState_INIT_REINDEX;
    case SynchronizationState::INIT_DOWNLOAD:
        return cck_SynchronizationState_INIT_DOWNLOAD;
    case SynchronizationState::POST_INIT:
        return cck_SynchronizationState_POST_INIT;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

cck_Warning cast_cck_warning(kernel::Warning warning)
{
    switch (warning) {
    case kernel::Warning::UNKNOWN_NEW_RULES_ACTIVATED:
        return cck_Warning_UNKNOWN_NEW_RULES_ACTIVATED;
    case kernel::Warning::LARGE_WORK_INVALID_CHAIN:
        return cck_Warning_LARGE_WORK_INVALID_CHAIN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

struct LoggingConnection {
    std::unique_ptr<std::list<std::function<void(const std::string&)>>::iterator> m_connection;
    void* m_user_data;
    std::function<void(void* user_data)> m_deleter;

    LoggingConnection(cck_LogCallback callback, void* user_data, cck_DestroyCallback user_data_destroy_callback)
    {
        LOCK(cs_main);

        auto connection{LogInstance().PushBackCallback([callback, user_data](const std::string& str) { callback(user_data, str.c_str(), str.length()); })};

        // Only start logging if we just added the connection.
        if (LogInstance().NumConnections() == 1 && !LogInstance().StartLogging()) {
            LogError("Logger start failed.");
            LogInstance().DeleteCallback(connection);
            if (user_data && user_data_destroy_callback) {
                user_data_destroy_callback(user_data);
            }
            throw std::runtime_error("Failed to start logging");
        }

        m_connection = std::make_unique<std::list<std::function<void(const std::string&)>>::iterator>(connection);
        m_user_data = user_data;
        m_deleter = user_data_destroy_callback;

        LogDebug(BCLog::KERNEL, "Logger connected.");
    }

    ~LoggingConnection()
    {
        LOCK(cs_main);
        LogDebug(BCLog::KERNEL, "Logger disconnecting.");

        // Switch back to buffering by calling DisconnectTestLogger if the
        // connection that we are about to remove is the last one.
        if (LogInstance().NumConnections() == 1) {
            LogInstance().DisconnectTestLogger();
        } else {
            LogInstance().DeleteCallback(*m_connection);
        }

        m_connection.reset();
        if (m_user_data && m_deleter) {
            m_deleter(m_user_data);
        }
    }
};

class KernelNotifications final : public kernel::Notifications
{
private:
    cck_NotificationInterfaceCallbacks m_cbs;

public:
    KernelNotifications(cck_NotificationInterfaceCallbacks cbs)
        : m_cbs{cbs}
    {
    }

    ~KernelNotifications()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data_destroy = nullptr;
        m_cbs.user_data = nullptr;
    }

    kernel::InterruptResult blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress) override
    {
        if (m_cbs.block_tip) m_cbs.block_tip(m_cbs.user_data, cast_state(state), cck_BlockTreeEntry::ref(&index), verification_progress);
        return {};
    }
    void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override
    {
        if (m_cbs.header_tip) m_cbs.header_tip(m_cbs.user_data, cast_state(state), height, timestamp, presync ? 1 : 0);
    }
    void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override
    {
        if (m_cbs.progress) m_cbs.progress(m_cbs.user_data, title.original.c_str(), title.original.length(), progress_percent, resume_possible ? 1 : 0);
    }
    void warningSet(kernel::Warning id, const bilingual_str& message) override
    {
        if (m_cbs.warning_set) m_cbs.warning_set(m_cbs.user_data, cast_cck_warning(id), message.original.c_str(), message.original.length());
    }
    void warningUnset(kernel::Warning id) override
    {
        if (m_cbs.warning_unset) m_cbs.warning_unset(m_cbs.user_data, cast_cck_warning(id));
    }
    void flushError(const bilingual_str& message) override
    {
        if (m_cbs.flush_error) m_cbs.flush_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
    void fatalError(const bilingual_str& message) override
    {
        if (m_cbs.fatal_error) m_cbs.fatal_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
};

class KernelValidationInterface final : public CValidationInterface
{
public:
    cck_ValidationInterfaceCallbacks m_cbs;

    explicit KernelValidationInterface(const cck_ValidationInterfaceCallbacks vi_cbs) : m_cbs{vi_cbs} {}

    ~KernelValidationInterface()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data = nullptr;
        m_cbs.user_data_destroy = nullptr;
    }

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& stateIn) override
    {
        if (m_cbs.block_checked) {
            m_cbs.block_checked(m_cbs.user_data,
                                cck_Block::copy(cck_Block::ref(&block)),
                                cck_BlockValidationState::ref(&stateIn));
        }
    }

    void NewPoWValidBlock(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block) override
    {
        if (m_cbs.pow_valid_block) {
            m_cbs.pow_valid_block(m_cbs.user_data,
                                  cck_Block::copy(cck_Block::ref(&block)),
                                  cck_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_connected) {
            m_cbs.block_connected(m_cbs.user_data,
                                  cck_Block::copy(cck_Block::ref(&block)),
                                  cck_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_disconnected) {
            m_cbs.block_disconnected(m_cbs.user_data,
                                     cck_Block::copy(cck_Block::ref(&block)),
                                     cck_BlockTreeEntry::ref(pindex));
        }
    }
};

struct ContextOptions {
    mutable Mutex m_mutex;
    std::unique_ptr<const CChainParams> m_chainparams GUARDED_BY(m_mutex);
    std::shared_ptr<KernelNotifications> m_notifications GUARDED_BY(m_mutex);
    std::shared_ptr<KernelValidationInterface> m_validation_interface GUARDED_BY(m_mutex);
};

class Context
{
public:
    std::unique_ptr<kernel::Context> m_context;

    std::shared_ptr<KernelNotifications> m_notifications;

    std::unique_ptr<util::SignalInterrupt> m_interrupt;

    std::unique_ptr<ValidationSignals> m_signals;

    std::unique_ptr<const CChainParams> m_chainparams;

    std::shared_ptr<KernelValidationInterface> m_validation_interface;

    Context(const ContextOptions* options, bool& sane)
        : m_context{std::make_unique<kernel::Context>()},
          m_interrupt{std::make_unique<util::SignalInterrupt>()}
    {
        if (options) {
            LOCK(options->m_mutex);
            if (options->m_chainparams) {
                m_chainparams = std::make_unique<const CChainParams>(*options->m_chainparams);
            }
            if (options->m_notifications) {
                m_notifications = options->m_notifications;
            }
            if (options->m_validation_interface) {
                m_signals = std::make_unique<ValidationSignals>(std::make_unique<ImmediateTaskRunner>());
                m_validation_interface = options->m_validation_interface;
                m_signals->RegisterSharedValidationInterface(m_validation_interface);
            }
        }

        if (!m_chainparams) {
            m_chainparams = CChainParams::Main();
        }
        if (!m_notifications) {
            m_notifications = std::make_shared<KernelNotifications>(cck_NotificationInterfaceCallbacks{
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
        }

        if (!kernel::SanityChecks(*m_context)) {
            sane = false;
        }
    }

    ~Context()
    {
        if (m_signals) {
            m_signals->UnregisterSharedValidationInterface(m_validation_interface);
        }
    }
};

//! Helper struct to wrap the ChainstateManager-related Options
struct ChainstateManagerOptions {
    mutable Mutex m_mutex;
    ChainstateManager::Options m_chainman_options GUARDED_BY(m_mutex);
    node::BlockManager::Options m_blockman_options GUARDED_BY(m_mutex);
    std::shared_ptr<const Context> m_context;
    node::ChainstateLoadOptions m_chainstate_load_options GUARDED_BY(m_mutex);
    uint64_t m_db_cache_bytes GUARDED_BY(m_mutex){DEFAULT_KERNEL_CACHE};

    ChainstateManagerOptions(const std::shared_ptr<const Context>& context, const fs::path& data_dir, const fs::path& blocks_dir)
        : m_chainman_options{ChainstateManager::Options{
              .chainparams = *context->m_chainparams,
              .datadir = data_dir,
              .notifications = *context->m_notifications,
              .signals = context->m_signals.get()}},
          m_blockman_options{node::BlockManager::Options{
              .chainparams = *context->m_chainparams,
              .blocks_dir = blocks_dir,
              .notifications = *context->m_notifications,
              .block_tree_db_params = DBParams{
                  .path = data_dir / "blocks" / "index",
                  .cache_bytes = kernel::CacheSizes{DEFAULT_KERNEL_CACHE}.block_tree_db,
              }}},
          m_context{context}, m_chainstate_load_options{node::ChainstateLoadOptions{}}
    {
    }
};

struct ChainMan {
    std::unique_ptr<ChainstateManager> m_chainman;
    std::shared_ptr<const Context> m_context;

    ChainMan(std::unique_ptr<ChainstateManager> chainman, std::shared_ptr<const Context> context)
        : m_chainman(std::move(chainman)), m_context(std::move(context)) {}
};

} // namespace

struct cck_Transaction : Handle<cck_Transaction, std::shared_ptr<const CTransaction>> {};
struct cck_TransactionOutput : Handle<cck_TransactionOutput, CTxOut> {};
struct cck_ScriptPubkey : Handle<cck_ScriptPubkey, CScript> {};
struct cck_LoggingConnection : Handle<cck_LoggingConnection, LoggingConnection> {};
struct cck_ContextOptions : Handle<cck_ContextOptions, ContextOptions> {};
struct cck_Context : Handle<cck_Context, std::shared_ptr<const Context>> {};
struct cck_ChainParameters : Handle<cck_ChainParameters, CChainParams> {};
struct cck_ChainstateManagerOptions : Handle<cck_ChainstateManagerOptions, ChainstateManagerOptions> {};
struct cck_ChainstateManager : Handle<cck_ChainstateManager, ChainMan> {};
struct cck_Chain : Handle<cck_Chain, CChain> {};
struct cck_BlockSpentOutputs : Handle<cck_BlockSpentOutputs, std::shared_ptr<CBlockUndo>> {};
struct cck_TransactionSpentOutputs : Handle<cck_TransactionSpentOutputs, CTxUndo> {};
struct cck_Coin : Handle<cck_Coin, Coin> {};
struct cck_BlockHash : Handle<cck_BlockHash, uint256> {};
struct cck_TransactionInput : Handle<cck_TransactionInput, CTxIn> {};
struct cck_WitnessStack : Handle<cck_WitnessStack, CScriptWitness> {};
struct cck_TransactionOutPoint: Handle<cck_TransactionOutPoint, COutPoint> {};
struct cck_Txid: Handle<cck_Txid, Txid> {};
struct cck_PrecomputedTransactionData : Handle<cck_PrecomputedTransactionData, PrecomputedTransactionData> {};
struct cck_BlockHeader: Handle<cck_BlockHeader, CBlockHeader> {};
struct cck_ConsensusParams: Handle<cck_ConsensusParams, Consensus::Params> {};

cck_Transaction* cck_transaction_create(const void* raw_transaction, size_t raw_transaction_len)
{
    assert(raw_transaction != nullptr || raw_transaction_len == 0);
    try {
        SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_transaction), raw_transaction_len}};
        return cck_Transaction::create(std::make_shared<const CTransaction>(deserialize, TX_WITH_WITNESS, stream));
    } catch (...) {
        return nullptr;
    }
}

size_t cck_transaction_count_outputs(const cck_Transaction* transaction)
{
    return cck_Transaction::get(transaction)->vout.size();
}

const cck_TransactionOutput* cck_transaction_get_output_at(const cck_Transaction* transaction, size_t output_index)
{
    const CTransaction& tx = *cck_Transaction::get(transaction);
    assert(output_index < tx.vout.size());
    return cck_TransactionOutput::ref(&tx.vout[output_index]);
}

size_t cck_transaction_count_inputs(const cck_Transaction* transaction)
{
    return cck_Transaction::get(transaction)->vin.size();
}

const cck_TransactionInput* cck_transaction_get_input_at(const cck_Transaction* transaction, size_t input_index)
{
    assert(input_index < cck_Transaction::get(transaction)->vin.size());
    return cck_TransactionInput::ref(&cck_Transaction::get(transaction)->vin[input_index]);
}

uint32_t cck_transaction_get_locktime(const cck_Transaction* transaction)
{
    return cck_Transaction::get(transaction)->nLockTime;
}

const cck_Txid* cck_transaction_get_txid(const cck_Transaction* transaction)
{
    return cck_Txid::ref(&cck_Transaction::get(transaction)->GetHash());
}

cck_Transaction* cck_transaction_copy(const cck_Transaction* transaction)
{
    return cck_Transaction::copy(transaction);
}

int cck_transaction_to_bytes(const cck_Transaction* transaction, cck_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(cck_Transaction::get(transaction));
        return 0;
    } catch (...) {
        return -1;
    }
}

void cck_transaction_destroy(cck_Transaction* transaction)
{
    delete transaction;
}

cck_ScriptPubkey* cck_script_pubkey_create(const void* script_pubkey, size_t script_pubkey_len)
{
    assert(script_pubkey != nullptr || script_pubkey_len == 0);
    auto data = std::span{reinterpret_cast<const uint8_t*>(script_pubkey), script_pubkey_len};
    return cck_ScriptPubkey::create(data.begin(), data.end());
}

int cck_script_pubkey_to_bytes(const cck_ScriptPubkey* script_pubkey_, cck_WriteBytes writer, void* user_data)
{
    const auto& script_pubkey{cck_ScriptPubkey::get(script_pubkey_)};
    return writer(script_pubkey.data(), script_pubkey.size(), user_data);
}

cck_ScriptPubkey* cck_script_pubkey_copy(const cck_ScriptPubkey* script_pubkey)
{
    return cck_ScriptPubkey::copy(script_pubkey);
}

void cck_script_pubkey_destroy(cck_ScriptPubkey* script_pubkey)
{
    delete script_pubkey;
}

cck_TransactionOutput* cck_transaction_output_create(const cck_ScriptPubkey* script_pubkey, int64_t amount)
{
    CTxOut output{amount, cck_ScriptPubkey::get(script_pubkey)};
    if (output.GetType() != TxOutputType::P2PK || !output.GetP2PKPubKey()) {
        return nullptr;
    }
    return cck_TransactionOutput::create(std::move(output));
}

cck_TransactionOutput* cck_transaction_output_copy(const cck_TransactionOutput* output)
{
    return cck_TransactionOutput::copy(output);
}

const cck_ScriptPubkey* cck_transaction_output_get_script_pubkey(const cck_TransactionOutput* output)
{
    return cck_ScriptPubkey::ref(&cck_TransactionOutput::get(output).scriptPubKey);
}

int64_t cck_transaction_output_get_amount(const cck_TransactionOutput* output)
{
    return cck_TransactionOutput::get(output).nValue;
}

void cck_transaction_output_destroy(cck_TransactionOutput* output)
{
    delete output;
}

cck_PrecomputedTransactionData* cck_precomputed_transaction_data_create(
    const cck_Transaction* tx_to,
    const cck_TransactionOutput** spent_outputs_, size_t spent_outputs_len)
{
    try {
        const CTransaction& tx{*cck_Transaction::get(tx_to)};
        auto txdata{cck_PrecomputedTransactionData::create()};
        if (spent_outputs_ != nullptr && spent_outputs_len > 0) {
            assert(spent_outputs_len == tx.vin.size());
            std::vector<CTxOut> spent_outputs;
            spent_outputs.reserve(spent_outputs_len);
            for (size_t i = 0; i < spent_outputs_len; i++) {
                const CTxOut& tx_out{cck_TransactionOutput::get(spent_outputs_[i])};
                spent_outputs.push_back(tx_out);
            }
            cck_PrecomputedTransactionData::get(txdata).Init(tx, std::move(spent_outputs));
        } else {
            cck_PrecomputedTransactionData::get(txdata).Init(tx, {});
        }

        return txdata;
    } catch (...) {
        return nullptr;
    }
}

cck_PrecomputedTransactionData* cck_precomputed_transaction_data_copy(const cck_PrecomputedTransactionData* precomputed_txdata)
{
    return cck_PrecomputedTransactionData::copy(precomputed_txdata);
}

void cck_precomputed_transaction_data_destroy(cck_PrecomputedTransactionData* precomputed_txdata)
{
    delete precomputed_txdata;
}

int cck_script_pubkey_verify(const cck_ScriptPubkey* script_pubkey,
                              const int64_t amount,
                              const cck_Transaction* tx_to,
                              const cck_PrecomputedTransactionData* precomputed_txdata,
                              const unsigned int input_index,
                              const cck_ScriptVerificationFlags flags,
                              cck_ScriptVerifyStatus* status)
{
    // Assert that all specified flags are part of the interface before continuing
    assert((flags & ~cck_ScriptVerificationFlags_ALL) == 0);

    if (!is_valid_flag_combination(script_verify_flags::from_int(flags))) {
        if (status) *status = cck_ScriptVerifyStatus_ERROR_INVALID_FLAGS_COMBINATION;
        return 0;
    }

    const CTransaction& tx{*cck_Transaction::get(tx_to)};
    assert(input_index < tx.vin.size());

    if (!precomputed_txdata) {
        if (status) *status = cck_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED;
        return 0;
    }
    const PrecomputedTransactionData& txdata{cck_PrecomputedTransactionData::get(precomputed_txdata)};
    if (!txdata.m_spent_outputs_ready || txdata.m_spent_outputs.size() != tx.vin.size()) {
        if (status) *status = cck_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED;
        return 0;
    }

    if (status) *status = cck_ScriptVerifyStatus_OK;

    const CTxOut spent_output{amount, cck_ScriptPubkey::get(script_pubkey)};
    const auto pubkey{spent_output.GetP2PKPubKey()};
    const CTxIn& txin{tx.vin[input_index]};
    if (!pubkey || txdata.m_spent_outputs[input_index] != spent_output ||
        !txin.scriptSig.empty() || txin.scriptWitness.stack.size() != 1 ||
        txin.scriptWitness.stack.front().size() != 64) {
        return 0;
    }

    ScriptExecutionData execdata;
    execdata.m_annex_init = true;
    execdata.m_annex_present = false;
    ScriptError error{SCRIPT_ERR_OK};
    TransactionSignatureChecker checker{&tx, input_index, amount, txdata, MissingDataBehavior::FAIL};
    return checker.CheckSchnorrSignature(txin.scriptWitness.stack.front(),
                                         std::span<const unsigned char>{pubkey->data(), pubkey->size()},
                                         SigVersion::TAPROOT, execdata, &error)
        ? 1
        : 0;
}

cck_TransactionInput* cck_transaction_input_copy(const cck_TransactionInput* input)
{
    return cck_TransactionInput::copy(input);
}

const cck_TransactionOutPoint* cck_transaction_input_get_out_point(const cck_TransactionInput* input)
{
    return cck_TransactionOutPoint::ref(&cck_TransactionInput::get(input).prevout);
}

uint32_t cck_transaction_input_get_sequence(const cck_TransactionInput* input)
{
    return cck_TransactionInput::get(input).nSequence;
}

const cck_WitnessStack* cck_transaction_input_get_witness_stack(const cck_TransactionInput* input)
{
    return cck_WitnessStack::ref(&cck_TransactionInput::get(input).scriptWitness);
}

int cck_transaction_input_get_script_sig(const cck_TransactionInput* input, cck_WriteBytes writer, void* user_data)
{
    const auto& script_sig{cck_TransactionInput::get(input).scriptSig};
    return writer(script_sig.data(), script_sig.size(), user_data);
}

void cck_transaction_input_destroy(cck_TransactionInput* input)
{
    delete input;
}

size_t cck_witness_stack_count_items(const cck_WitnessStack* witness_stack)
{
    return cck_WitnessStack::get(witness_stack).stack.size();
}

int cck_witness_stack_get_item_at(const cck_WitnessStack* witness_stack, size_t index, cck_WriteBytes writer, void* user_data)
{
    const auto& stack{cck_WitnessStack::get(witness_stack).stack};
    assert(index < stack.size());
    return writer(stack[index].data(), stack[index].size(), user_data);
}

cck_WitnessStack* cck_witness_stack_copy(const cck_WitnessStack* witness_stack)
{
    return cck_WitnessStack::copy(witness_stack);
}

void cck_witness_stack_destroy(cck_WitnessStack* witness_stack)
{
    delete witness_stack;
}

cck_TransactionOutPoint* cck_transaction_out_point_copy(const cck_TransactionOutPoint* out_point)
{
    return cck_TransactionOutPoint::copy(out_point);
}

uint32_t cck_transaction_out_point_get_index(const cck_TransactionOutPoint* out_point)
{
    return cck_TransactionOutPoint::get(out_point).n;
}

const cck_Txid* cck_transaction_out_point_get_txid(const cck_TransactionOutPoint* out_point)
{
    return cck_Txid::ref(&cck_TransactionOutPoint::get(out_point).hash);
}

void cck_transaction_out_point_destroy(cck_TransactionOutPoint* out_point)
{
    delete out_point;
}

cck_Txid* cck_txid_copy(const cck_Txid* txid)
{
    return cck_Txid::copy(txid);
}

void cck_txid_to_bytes(const cck_Txid* txid, unsigned char output[32])
{
    std::memcpy(output, cck_Txid::get(txid).begin(), 32);
}

int cck_txid_equals(const cck_Txid* txid1, const cck_Txid* txid2)
{
    return cck_Txid::get(txid1) == cck_Txid::get(txid2);
}

void cck_txid_destroy(cck_Txid* txid)
{
    delete txid;
}

void cck_logging_set_options(const cck_LoggingOptions options)
{
    LOCK(cs_main);
    LogInstance().m_log_timestamps = options.log_timestamps;
    LogInstance().m_log_time_micros = options.log_time_micros;
    LogInstance().m_log_threadnames = options.log_threadnames;
    LogInstance().m_log_sourcelocations = options.log_sourcelocations;
    LogInstance().m_always_print_category_level = options.always_print_category_levels;
}

void cck_logging_set_level_category(cck_LogCategory category, cck_LogLevel level)
{
    LOCK(cs_main);
    if (category == cck_LogCategory_ALL) {
        LogInstance().SetLogLevel(get_bclog_level(level));
    }

    LogInstance().AddCategoryLogLevel(get_bclog_flag(category), get_bclog_level(level));
}

void cck_logging_enable_category(cck_LogCategory category)
{
    LogInstance().EnableCategory(get_bclog_flag(category));
}

void cck_logging_disable_category(cck_LogCategory category)
{
    LogInstance().DisableCategory(get_bclog_flag(category));
}

void cck_logging_disable()
{
    LogInstance().DisableLogging();
}

cck_LoggingConnection* cck_logging_connection_create(cck_LogCallback callback, void* user_data, cck_DestroyCallback user_data_destroy_callback)
{
    try {
        return cck_LoggingConnection::create(callback, user_data, user_data_destroy_callback);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void cck_logging_connection_destroy(cck_LoggingConnection* connection)
{
    delete connection;
}

cck_ChainParameters* cck_chain_parameters_create(const cck_ChainType chain_type)
{
    switch (chain_type) {
    case cck_ChainType_MAINNET: {
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::Main().release()));
    }
    case cck_ChainType_TESTNET: {
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet().release()));
    }
    case cck_ChainType_TESTNET_4: {
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet4().release()));
    }
    case cck_ChainType_SIGNET: {
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::SigNet().release()));
    }
    case cck_ChainType_REGTEST: {
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::RegTest().release()));
    }
    }
    assert(false);
}

cck_ChainParameters* cck_chain_parameters_create_signet(const void* challenge, size_t challenge_len)
{
    assert(challenge != nullptr || challenge_len == 0);
    const uint8_t* p = static_cast<const uint8_t*>(challenge);
    try {
        std::vector<uint8_t> challenge_bytes;
        if (challenge_len != 0) {
            challenge_bytes.assign(p, p + challenge_len);
        }
        CChainParams::SigNetOptions options{
            .challenge = std::move(challenge_bytes),
        };
        return cck_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::SigNet(options).release()));
    } catch (...) {
        return nullptr;
    }
}

cck_ChainParameters* cck_chain_parameters_copy(const cck_ChainParameters* chain_parameters)
{
    return cck_ChainParameters::copy(chain_parameters);
}

const cck_ConsensusParams* cck_chain_parameters_get_consensus_params(const cck_ChainParameters* chain_parameters)
{
    return cck_ConsensusParams::ref(&cck_ChainParameters::get(chain_parameters).GetConsensus());
}

void cck_chain_parameters_destroy(cck_ChainParameters* chain_parameters)
{
    delete chain_parameters;
}

cck_ContextOptions* cck_context_options_create()
{
    return cck_ContextOptions::create();
}

void cck_context_options_set_chainparams(cck_ContextOptions* options, const cck_ChainParameters* chain_parameters)
{
    // Copy the chainparams, so the caller can free it again
    LOCK(cck_ContextOptions::get(options).m_mutex);
    cck_ContextOptions::get(options).m_chainparams = std::make_unique<const CChainParams>(cck_ChainParameters::get(chain_parameters));
}

void cck_context_options_set_notifications(cck_ContextOptions* options, cck_NotificationInterfaceCallbacks notifications)
{
    // The KernelNotifications are copy-initialized, so the caller can free them again.
    LOCK(cck_ContextOptions::get(options).m_mutex);
    cck_ContextOptions::get(options).m_notifications = std::make_shared<KernelNotifications>(notifications);
}

void cck_context_options_set_validation_interface(cck_ContextOptions* options, cck_ValidationInterfaceCallbacks vi_cbs)
{
    LOCK(cck_ContextOptions::get(options).m_mutex);
    cck_ContextOptions::get(options).m_validation_interface = std::make_shared<KernelValidationInterface>(vi_cbs);
}

void cck_context_options_destroy(cck_ContextOptions* options)
{
    delete options;
}

cck_Context* cck_context_create(const cck_ContextOptions* options)
{
    bool sane{true};
    const ContextOptions* opts = options ? &cck_ContextOptions::get(options) : nullptr;
    auto context{std::make_shared<const Context>(opts, sane)};
    if (!sane) {
        LogError("Kernel context sanity check failed.");
        return nullptr;
    }
    return cck_Context::create(context);
}

cck_Context* cck_context_copy(const cck_Context* context)
{
    return cck_Context::copy(context);
}

int cck_context_interrupt(cck_Context* context)
{
    return (*cck_Context::get(context)->m_interrupt)() ? 0 : -1;
}

void cck_context_destroy(cck_Context* context)
{
    delete context;
}

const cck_BlockTreeEntry* cck_block_tree_entry_get_previous(const cck_BlockTreeEntry* entry)
{
    if (!cck_BlockTreeEntry::get(entry).pprev) {
        LogInfo("Genesis block has no previous.");
        return nullptr;
    }

    return cck_BlockTreeEntry::ref(cck_BlockTreeEntry::get(entry).pprev);
}

const cck_BlockTreeEntry* cck_block_tree_entry_get_ancestor(const cck_BlockTreeEntry* block_tree_entry, int32_t height)
{
    const auto* ancestor{cck_BlockTreeEntry::get(block_tree_entry).GetAncestor(height)};
    assert(ancestor);
    return cck_BlockTreeEntry::ref(ancestor);
}

cck_BlockValidationState* cck_block_validation_state_create()
{
    return cck_BlockValidationState::create();
}

cck_BlockValidationState* cck_block_validation_state_copy(const cck_BlockValidationState* state)
{
    return cck_BlockValidationState::copy(state);
}

void cck_block_validation_state_destroy(cck_BlockValidationState* state)
{
    delete state;
}

cck_ValidationMode cck_block_validation_state_get_validation_mode(const cck_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = cck_BlockValidationState::get(block_validation_state_);
    if (block_validation_state.IsValid()) return cck_ValidationMode_VALID;
    if (block_validation_state.IsInvalid()) return cck_ValidationMode_INVALID;
    return cck_ValidationMode_INTERNAL_ERROR;
}

cck_BlockValidationResult cck_block_validation_state_get_block_validation_result(const cck_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = cck_BlockValidationState::get(block_validation_state_);
    switch (block_validation_state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        return cck_BlockValidationResult_UNSET;
    case BlockValidationResult::BLOCK_CONSENSUS:
        return cck_BlockValidationResult_CONSENSUS;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        return cck_BlockValidationResult_CACHED_INVALID;
    case BlockValidationResult::BLOCK_INVALID_HEADER:
        return cck_BlockValidationResult_INVALID_HEADER;
    case BlockValidationResult::BLOCK_MUTATED:
        return cck_BlockValidationResult_MUTATED;
    case BlockValidationResult::BLOCK_MISSING_PREV:
        return cck_BlockValidationResult_MISSING_PREV;
    case BlockValidationResult::BLOCK_INVALID_PREV:
        return cck_BlockValidationResult_INVALID_PREV;
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        return cck_BlockValidationResult_TIME_FUTURE;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        return cck_BlockValidationResult_HEADER_LOW_WORK;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

cck_ChainstateManagerOptions* cck_chainstate_manager_options_create(const cck_Context* context, const char* data_dir, size_t data_dir_len, const char* blocks_dir, size_t blocks_dir_len)
{
    assert(data_dir != nullptr || data_dir_len == 0);
    assert(blocks_dir != nullptr || blocks_dir_len == 0);
    if (data_dir_len == 0 || blocks_dir_len == 0) {
        LogError("Failed to create chainstate manager options: dir must be non-null and non-empty");
        return nullptr;
    }
    try {
        fs::path abs_data_dir{fs::absolute(fs::PathFromString({data_dir, data_dir_len}))};
        fs::create_directories(abs_data_dir);
        fs::path abs_blocks_dir{fs::absolute(fs::PathFromString({blocks_dir, blocks_dir_len}))};
        fs::create_directories(abs_blocks_dir);
        return cck_ChainstateManagerOptions::create(cck_Context::get(context), abs_data_dir, abs_blocks_dir);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager options: %s", e.what());
        return nullptr;
    }
}

void cck_chainstate_manager_options_set_worker_threads_num(cck_ChainstateManagerOptions* opts, int worker_threads)
{
    LOCK(cck_ChainstateManagerOptions::get(opts).m_mutex);
    cck_ChainstateManagerOptions::get(opts).m_chainman_options.worker_threads_num = worker_threads;
}

int cck_chainstate_manager_options_set_database_cache_bytes(cck_ChainstateManagerOptions* chainman_opts, uint64_t database_cache_bytes)
{
    if (database_cache_bytes < MIN_DBCACHE_BYTES || database_cache_bytes > MAX_DBCACHE_BYTES) {
        LogError("Failed to set database cache: size is outside the supported range.");
        return -1;
    }

    auto& opts{cck_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_db_cache_bytes = database_cache_bytes;
    opts.m_blockman_options.block_tree_db_params.cache_bytes = kernel::CacheSizes{database_cache_bytes}.block_tree_db;
    return 0;
}

void cck_chainstate_manager_options_destroy(cck_ChainstateManagerOptions* options)
{
    delete options;
}

int cck_chainstate_manager_options_set_wipe_dbs(cck_ChainstateManagerOptions* chainman_opts, int wipe_block_tree_db, int wipe_chainstate_db)
{
    if (wipe_block_tree_db == 1 && wipe_chainstate_db != 1) {
        LogError("Wiping the block tree db without also wiping the chainstate db is currently unsupported.");
        return -1;
    }
    auto& opts{cck_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.wipe_data = wipe_block_tree_db == 1;
    opts.m_chainstate_load_options.wipe_chainstate_db = wipe_chainstate_db == 1;
    return 0;
}

void cck_chainstate_manager_options_update_block_tree_db_in_memory(
    cck_ChainstateManagerOptions* chainman_opts,
    int block_tree_db_in_memory)
{
    auto& opts{cck_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.memory_only = block_tree_db_in_memory == 1;
}

void cck_chainstate_manager_options_update_chainstate_db_in_memory(
    cck_ChainstateManagerOptions* chainman_opts,
    int chainstate_db_in_memory)
{
    auto& opts{cck_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_chainstate_load_options.coins_db_in_memory = chainstate_db_in_memory == 1;
}

cck_ChainstateManager* cck_chainstate_manager_create(
    const cck_ChainstateManagerOptions* chainman_opts)
{
    auto& opts{cck_ChainstateManagerOptions::get(chainman_opts)};
    std::unique_ptr<ChainstateManager> chainman;
    try {
        LOCK(opts.m_mutex);
        chainman = std::make_unique<ChainstateManager>(*opts.m_context->m_interrupt, opts.m_chainman_options, opts.m_blockman_options);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager: %s", e.what());
        return nullptr;
    }

    try {
        const auto chainstate_load_opts{WITH_LOCK(opts.m_mutex, return opts.m_chainstate_load_options)};

        const kernel::CacheSizes cache_sizes{WITH_LOCK(opts.m_mutex, return opts.m_db_cache_bytes)};
        auto [status, chainstate_err]{node::LoadChainstate(*chainman, cache_sizes, chainstate_load_opts)};
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to load chain state from your data directory: %s", chainstate_err.original);
            return nullptr;
        }
        std::tie(status, chainstate_err) = node::VerifyLoadedChainstate(*chainman, chainstate_load_opts);
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to verify loaded chain state from your datadir: %s", chainstate_err.original);
            return nullptr;
        }
        if (auto result = chainman->ActivateBestChains(); !result) {
            LogError("%s", util::ErrorString(result).original);
            return nullptr;
        }
    } catch (const std::exception& e) {
        LogError("Failed to load chainstate: %s", e.what());
        return nullptr;
    }

    return cck_ChainstateManager::create(std::move(chainman), opts.m_context);
}

const cck_BlockTreeEntry* cck_chainstate_manager_get_block_tree_entry_by_hash(const cck_ChainstateManager* chainman, const cck_BlockHash* block_hash)
{
    auto block_index = WITH_LOCK(cck_ChainstateManager::get(chainman).m_chainman->GetMutex(),
                                 return cck_ChainstateManager::get(chainman).m_chainman->m_blockman.LookupBlockIndex(cck_BlockHash::get(block_hash)));
    if (!block_index) {
        LogDebug(BCLog::KERNEL, "A block with the given hash is not indexed.");
        return nullptr;
    }
    return cck_BlockTreeEntry::ref(block_index);
}

const cck_BlockTreeEntry* cck_chainstate_manager_get_best_entry(const cck_ChainstateManager* chainstate_manager)
{
    auto& chainman = *cck_ChainstateManager::get(chainstate_manager).m_chainman;
    return cck_BlockTreeEntry::ref(WITH_LOCK(chainman.GetMutex(), return chainman.m_best_header));
}

void cck_chainstate_manager_destroy(cck_ChainstateManager* chainman)
{
    {
        LOCK(cck_ChainstateManager::get(chainman).m_chainman->GetMutex());
        for (const auto& chainstate : cck_ChainstateManager::get(chainman).m_chainman->m_chainstates) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }

    delete chainman;
}

int cck_chainstate_manager_import_blocks(cck_ChainstateManager* chainman, const char** block_file_paths_data, size_t* block_file_paths_lens, size_t block_file_paths_data_len)
{
    try {
        std::vector<fs::path> import_files;
        import_files.reserve(block_file_paths_data_len);
        for (uint32_t i = 0; i < block_file_paths_data_len; i++) {
            if (block_file_paths_data[i] != nullptr) {
                import_files.emplace_back(std::string{block_file_paths_data[i], block_file_paths_lens[i]}.c_str());
            }
        }
        auto& chainman_ref{*cck_ChainstateManager::get(chainman).m_chainman};
        node::ImportBlocks(chainman_ref, import_files);
        WITH_LOCK(::cs_main, chainman_ref.UpdateIBDStatus());
    } catch (const std::exception& e) {
        LogError("Failed to import blocks: %s", e.what());
        return -1;
    }
    return 0;
}

cck_Block* cck_block_create(const void* raw_block, size_t raw_block_length)
{
    assert(raw_block != nullptr || raw_block_length == 0);
    auto block{std::make_shared<CBlock>()};

    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block), raw_block_length}};

    try {
        stream >> TX_WITH_WITNESS(*block);
    } catch (...) {
        LogDebug(BCLog::KERNEL, "Block decode failed.");
        return nullptr;
    }

    return cck_Block::create(block);
}

cck_Block* cck_block_copy(const cck_Block* block)
{
    return cck_Block::copy(block);
}

int cck_block_check(const cck_Block* block, const cck_ConsensusParams* consensus_params, cck_BlockCheckFlags flags, cck_BlockValidationState* validation_state)
{
    auto& state = cck_BlockValidationState::get(validation_state);
    state = BlockValidationState{};

    const bool check_pow    = (flags & cck_BlockCheckFlags_POW) != 0;
    const bool check_merkle = (flags & cck_BlockCheckFlags_MERKLE) != 0;

    const bool result = CheckBlock(*cck_Block::get(block), state, cck_ConsensusParams::get(consensus_params), /*fCheckPOW=*/check_pow, /*fCheckMerkleRoot=*/check_merkle);

    return result ? 1 : 0;
}

size_t cck_block_count_transactions(const cck_Block* block)
{
    return cck_Block::get(block)->vtx.size();
}

const cck_Transaction* cck_block_get_transaction_at(const cck_Block* block, size_t index)
{
    assert(index < cck_Block::get(block)->vtx.size());
    return cck_Transaction::ref(&cck_Block::get(block)->vtx[index]);
}

cck_BlockHeader* cck_block_get_header(const cck_Block* block)
{
    const auto& block_ptr = cck_Block::get(block);
    return cck_BlockHeader::create(static_cast<const CBlockHeader&>(*block_ptr));
}

int cck_block_to_bytes(const cck_Block* block, cck_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(*cck_Block::get(block));
        return 0;
    } catch (...) {
        return -1;
    }
}

cck_BlockHash* cck_block_get_hash(const cck_Block* block)
{
    return cck_BlockHash::create(cck_Block::get(block)->GetHash());
}

void cck_block_destroy(cck_Block* block)
{
    delete block;
}

cck_Block* cck_block_read(const cck_ChainstateManager* chainman, const cck_BlockTreeEntry* entry)
{
    auto block{std::make_shared<CBlock>()};
    if (!cck_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlock(*block, cck_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block.");
        return nullptr;
    }
    return cck_Block::create(block);
}

cck_BlockHeader* cck_block_tree_entry_get_block_header(const cck_BlockTreeEntry* entry)
{
    return cck_BlockHeader::create(cck_BlockTreeEntry::get(entry).GetBlockHeader());
}

int32_t cck_block_tree_entry_get_height(const cck_BlockTreeEntry* entry)
{
    return cck_BlockTreeEntry::get(entry).nHeight;
}

const cck_BlockHash* cck_block_tree_entry_get_block_hash(const cck_BlockTreeEntry* entry)
{
    return cck_BlockHash::ref(cck_BlockTreeEntry::get(entry).phashBlock);
}

int cck_block_tree_entry_equals(const cck_BlockTreeEntry* entry1, const cck_BlockTreeEntry* entry2)
{
    return &cck_BlockTreeEntry::get(entry1) == &cck_BlockTreeEntry::get(entry2);
}

cck_BlockHash* cck_block_hash_create(const unsigned char block_hash[32])
{
    return cck_BlockHash::create(std::span<const unsigned char>{block_hash, 32});
}

cck_BlockHash* cck_block_hash_copy(const cck_BlockHash* block_hash)
{
    return cck_BlockHash::copy(block_hash);
}

void cck_block_hash_to_bytes(const cck_BlockHash* block_hash, unsigned char output[32])
{
    std::memcpy(output, cck_BlockHash::get(block_hash).begin(), 32);
}

int cck_block_hash_equals(const cck_BlockHash* hash1, const cck_BlockHash* hash2)
{
    return cck_BlockHash::get(hash1) == cck_BlockHash::get(hash2);
}

void cck_block_hash_destroy(cck_BlockHash* hash)
{
    delete hash;
}

cck_BlockSpentOutputs* cck_block_spent_outputs_read(const cck_ChainstateManager* chainman, const cck_BlockTreeEntry* entry)
{
    auto block_undo{std::make_shared<CBlockUndo>()};
    if (cck_BlockTreeEntry::get(entry).nHeight < 1) {
        LogDebug(BCLog::KERNEL, "The genesis block does not have any spent outputs.");
        return cck_BlockSpentOutputs::create(block_undo);
    }
    if (!cck_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlockUndo(*block_undo, cck_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block spent outputs data.");
        return nullptr;
    }
    return cck_BlockSpentOutputs::create(block_undo);
}

cck_BlockSpentOutputs* cck_block_spent_outputs_copy(const cck_BlockSpentOutputs* block_spent_outputs)
{
    return cck_BlockSpentOutputs::copy(block_spent_outputs);
}

size_t cck_block_spent_outputs_count(const cck_BlockSpentOutputs* block_spent_outputs)
{
    return cck_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size();
}

const cck_TransactionSpentOutputs* cck_block_spent_outputs_get_transaction_spent_outputs_at(const cck_BlockSpentOutputs* block_spent_outputs, size_t transaction_index)
{
    assert(transaction_index < cck_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size());
    const auto* tx_undo{&cck_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.at(transaction_index)};
    return cck_TransactionSpentOutputs::ref(tx_undo);
}

void cck_block_spent_outputs_destroy(cck_BlockSpentOutputs* block_spent_outputs)
{
    delete block_spent_outputs;
}

cck_TransactionSpentOutputs* cck_transaction_spent_outputs_copy(const cck_TransactionSpentOutputs* transaction_spent_outputs)
{
    return cck_TransactionSpentOutputs::copy(transaction_spent_outputs);
}

size_t cck_transaction_spent_outputs_count(const cck_TransactionSpentOutputs* transaction_spent_outputs)
{
    return cck_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size();
}

void cck_transaction_spent_outputs_destroy(cck_TransactionSpentOutputs* transaction_spent_outputs)
{
    delete transaction_spent_outputs;
}

const cck_Coin* cck_transaction_spent_outputs_get_coin_at(const cck_TransactionSpentOutputs* transaction_spent_outputs, size_t coin_index)
{
    assert(coin_index < cck_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size());
    const Coin* coin{&cck_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.at(coin_index)};
    return cck_Coin::ref(coin);
}

cck_Coin* cck_coin_copy(const cck_Coin* coin)
{
    return cck_Coin::copy(coin);
}

uint32_t cck_coin_confirmation_height(const cck_Coin* coin)
{
    return cck_Coin::get(coin).nHeight;
}

int cck_coin_is_coinbase(const cck_Coin* coin)
{
    return cck_Coin::get(coin).IsCoinBase() ? 1 : 0;
}

const cck_TransactionOutput* cck_coin_get_output(const cck_Coin* coin)
{
    return cck_TransactionOutput::ref(&cck_Coin::get(coin).out);
}

void cck_coin_destroy(cck_Coin* coin)
{
    delete coin;
}

int cck_chainstate_manager_process_block(
    cck_ChainstateManager* chainman,
    const cck_Block* block,
    int* _new_block)
{
    bool new_block;
    auto result = cck_ChainstateManager::get(chainman).m_chainman->ProcessNewBlock(cck_Block::get(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    if (_new_block) {
        *_new_block = new_block ? 1 : 0;
    }
    return result ? 0 : -1;
}

cck_BlockValidationState* cck_chainstate_manager_process_block_header(
    cck_ChainstateManager* chainstate_manager,
    const cck_BlockHeader* header)
{
    try {
        auto& chainman = cck_ChainstateManager::get(chainstate_manager).m_chainman;

        auto state = cck_BlockValidationState::create();
        bool result{chainman->ProcessNewBlockHeaders({&cck_BlockHeader::get(header), 1}, /*min_pow_checked=*/true, cck_BlockValidationState::get(state))};
        assert(result == cck_BlockValidationState::get(state).IsValid());
        return state;
    } catch (const std::exception& e) {
        LogError("Failed to process block header: %s", e.what());
        return nullptr;
    }
}

const cck_Chain* cck_chainstate_manager_get_active_chain(const cck_ChainstateManager* chainman)
{
    return cck_Chain::ref(&WITH_LOCK(cck_ChainstateManager::get(chainman).m_chainman->GetMutex(), return cck_ChainstateManager::get(chainman).m_chainman->ActiveChain()));
}

int32_t cck_chain_get_height(const cck_Chain* chain)
{
    LOCK(::cs_main);
    return cck_Chain::get(chain).Height();
}

const cck_BlockTreeEntry* cck_chain_get_by_height(const cck_Chain* chain, int32_t height)
{
    LOCK(::cs_main);
    return cck_BlockTreeEntry::ref(cck_Chain::get(chain)[height]);
}

int cck_chain_contains(const cck_Chain* chain, const cck_BlockTreeEntry* entry)
{
    LOCK(::cs_main);
    return cck_Chain::get(chain).Contains(cck_BlockTreeEntry::get(entry)) ? 1 : 0;
}

cck_BlockHeader* cck_block_header_create(const void* raw_block_header, size_t raw_block_header_len)
{
    assert(raw_block_header != nullptr && raw_block_header_len == 80);
    auto header{std::make_unique<CBlockHeader>()};
    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block_header), raw_block_header_len}};

    try {
        stream >> *header;
    } catch (...) {
        LogError("Block header decode failed.");
        return nullptr;
    }

    return cck_BlockHeader::ref(header.release());
}

cck_BlockHeader* cck_block_header_copy(const cck_BlockHeader* header)
{
    return cck_BlockHeader::copy(header);
}

cck_BlockHash* cck_block_header_get_hash(const cck_BlockHeader* header)
{
    return cck_BlockHash::create(cck_BlockHeader::get(header).GetHash());
}

const cck_BlockHash* cck_block_header_get_prev_hash(const cck_BlockHeader* header)
{
    return cck_BlockHash::ref(&cck_BlockHeader::get(header).hashPrevBlock);
}

uint32_t cck_block_header_get_timestamp(const cck_BlockHeader* header)
{
    return cck_BlockHeader::get(header).nTime;
}

uint32_t cck_block_header_get_bits(const cck_BlockHeader* header)
{
    return cck_BlockHeader::get(header).nBits;
}

int32_t cck_block_header_get_version(const cck_BlockHeader* header)
{
    return cck_BlockHeader::get(header).nVersion;
}

uint32_t cck_block_header_get_nonce(const cck_BlockHeader* header)
{
    return cck_BlockHeader::get(header).nNonce;
}

int cck_block_header_to_bytes(const cck_BlockHeader* header, unsigned char output[80])
{
    try {
        SpanWriter{std::as_writable_bytes(std::span{output, 80})} << cck_BlockHeader::get(header);
        return 0;
    } catch (...) {
        return -1;
    }
}

void cck_block_header_destroy(cck_BlockHeader* header)
{
    delete header;
}

cck_ValidationMode cck_tx_validation_state_get_validation_mode(const cck_TxValidationState* state_)
{
    const auto& state = cck_TxValidationState::get(state_);
    if (state.IsValid()) return cck_ValidationMode_VALID;
    if (state.IsInvalid()) return cck_ValidationMode_INVALID;
    return cck_ValidationMode_INTERNAL_ERROR;
}

cck_TxValidationState* cck_tx_validation_state_create()
{
    return cck_TxValidationState::create();
}

cck_TxValidationResult cck_tx_validation_state_get_tx_validation_result(const cck_TxValidationState* state_)
{
    switch (cck_TxValidationState::get(state_).GetResult()) {
    case TxValidationResult::TX_RESULT_UNSET:        return cck_TxValidationResult_UNSET;
    case TxValidationResult::TX_CONSENSUS:           return cck_TxValidationResult_CONSENSUS;
    case TxValidationResult::TX_INPUTS_NOT_STANDARD: return cck_TxValidationResult_INPUTS_NOT_STANDARD;
    case TxValidationResult::TX_NOT_STANDARD:        return cck_TxValidationResult_NOT_STANDARD;
    case TxValidationResult::TX_MISSING_INPUTS:      return cck_TxValidationResult_MISSING_INPUTS;
    case TxValidationResult::TX_PREMATURE_SPEND:     return cck_TxValidationResult_PREMATURE_SPEND;
    case TxValidationResult::TX_WITNESS_MUTATED:     return cck_TxValidationResult_WITNESS_MUTATED;
    case TxValidationResult::TX_WITNESS_STRIPPED:    return cck_TxValidationResult_WITNESS_STRIPPED;
    case TxValidationResult::TX_CONFLICT:            return cck_TxValidationResult_CONFLICT;
    case TxValidationResult::TX_MEMPOOL_POLICY:      return cck_TxValidationResult_MEMPOOL_POLICY;
    case TxValidationResult::TX_NO_MEMPOOL:          return cck_TxValidationResult_NO_MEMPOOL;
    case TxValidationResult::TX_RECONSIDERABLE:      return cck_TxValidationResult_RECONSIDERABLE;
    case TxValidationResult::TX_UNKNOWN:             return cck_TxValidationResult_UNKNOWN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

void cck_tx_validation_state_destroy(cck_TxValidationState* state)
{
    delete state;
}

int cck_transaction_check(const cck_Transaction* tx, cck_TxValidationState* validation_state)
{
    auto& state = cck_TxValidationState::get(validation_state);
    state = TxValidationState{};
    const bool ok = CheckTransaction(*cck_Transaction::get(tx), state);
    return ok ? 1 : 0;
}

int cck_set_mock_time(int64_t timestamp)
{
    constexpr int64_t max_time{std::numeric_limits<uint32_t>::max()};
    if (timestamp < 0 || timestamp > max_time) {
        return -1;
    }
    SetMockTime(std::chrono::seconds{timestamp});
    return 0;
}
