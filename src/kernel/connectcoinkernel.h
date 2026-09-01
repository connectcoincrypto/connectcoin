// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_KERNEL_CONNECTCOINKERNEL_H
#define CONNECTCOIN_KERNEL_CONNECTCOINKERNEL_H

#ifndef __cplusplus
#include <stddef.h>
#include <stdint.h>
#else
#include <cstddef>
#include <cstdint>
#endif // __cplusplus

#ifndef CONNECTCOINKERNEL_API
    #ifdef CONNECTCOINKERNEL_BUILD
        #if defined(_WIN32)
            #define CONNECTCOINKERNEL_API __declspec(dllexport)
        #else
            #define CONNECTCOINKERNEL_API __attribute__((visibility("default")))
        #endif
    #else
        #if defined(_WIN32) && !defined(CONNECTCOINKERNEL_STATIC)
            #define CONNECTCOINKERNEL_API __declspec(dllimport)
        #else
            #define CONNECTCOINKERNEL_API
        #endif
    #endif
#endif

/**
 * CONNECTCOINKERNEL_WARN_UNUSED_RESULT is a compiler attribute used to indicate
 * that ignoring a function's return value is almost certainly a bug.
 *
 * It is used in cases such as a resource leak (e.g. an owning handle returned
 * by a *_create or *_copy function), or when the returned value is itself an
 * error/status code. It is not used merely because discarding the result is
 * wasteful, e.g. on getters or predicates.
 */
#if defined(__GNUC__)
    #define CONNECTCOINKERNEL_WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
#else
    #define CONNECTCOINKERNEL_WARN_UNUSED_RESULT
#endif

/**
 * CONNECTCOINKERNEL_ARG_NONNULL is a compiler attribute used to indicate that
 * certain pointer arguments to a function are not expected to be null.
 *
 * Callers must not pass a null pointer for arguments marked with this attribute,
 * as doing so may result in undefined behavior. This attribute should only be
 * used for arguments where a null pointer is unambiguously a programmer error,
 * such as for opaque handles, and not for pointers to raw input data that might
 * validly be null (e.g., from an empty std::span or std::string).
 */
#if !defined(CONNECTCOINKERNEL_BUILD) && defined(__GNUC__)
    #define CONNECTCOINKERNEL_ARG_NONNULL(...) __attribute__((__nonnull__(__VA_ARGS__)))
#else
    #define CONNECTCOINKERNEL_ARG_NONNULL(...)
#endif

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @page remarks Remarks
 *
 * @section purpose Purpose
 *
 * This header currently exposes an API for interacting with parts of
 * ConnectCoin Core's consensus code. Users can validate blocks, iterate the block index,
 * read block and undo data from disk, and validate scripts. The header is
 * unversioned and not stable yet. Users should expect breaking changes. It is
 * also not yet included in releases of ConnectCoin Core.
 *
 * @section context Context
 *
 * The library provides a built-in static constant kernel context. This static
 * context offers only limited functionality. It detects and self-checks the
 * correct sha256 implementation, initializes the random number generator and
 * self-checks the secp256k1 static context. It is used internally for
 * otherwise "context-free" operations. This means that the user is not
 * required to initialize their own context before using the library.
 *
 * The user should create their own context for passing it to state-rich validation
 * functions and holding callbacks for kernel events.
 *
 * @section error Error handling
 *
 * Functions communicate an error through their return types, usually returning
 * a nullptr or a status code as documented by the returning function.
 * Additionally, verification functions, e.g. for scripts, may communicate more
 * detailed error information through status code out parameters.
 *
 * Fine-grained validation information is communicated through the validation
 * interface.
 *
 * The kernel notifications issue callbacks for errors. These are usually
 * indicative of a system error. If such an error is issued, it is recommended
 * to halt and tear down the existing kernel objects. Remediating the error may
 * require system intervention by the user.
 *
 * @section pointer Pointer and argument conventions
 *
 * The user is responsible for de-allocating the memory owned by pointers
 * returned by functions. Typically pointers returned by *_create(...) functions
 * can be de-allocated by corresponding *_destroy(...) functions.
 *
 * A function that takes pointer arguments makes no assumptions on their
 * lifetime. Once the function returns the user can safely de-allocate the
 * passed in arguments.
 *
 * Const pointers represent views, and do not transfer ownership. Lifetime
 * guarantees of these objects are described in the respective documentation.
 * Ownership of these resources may be taken by copying. They are typically
 * used for iteration with minimal overhead and require some care by the
 * programmer that their lifetime is not extended beyond that of the original
 * object.
 *
 * Array lengths follow the pointer argument they describe.
 *
 * @section types Type conventions
 *
 * Fixed-width integer types (e.g. int32_t, uint32_t) are used for data values
 * such as heights. Plain int and unsigned int are used for boolean-like values
 * and flags.
 */

/**
 * Opaque data structure for holding a transaction.
 */
typedef struct cck_Transaction cck_Transaction;

/**
 * Opaque data structure for holding a script pubkey.
 */
typedef struct cck_ScriptPubkey cck_ScriptPubkey;

/**
 * Opaque data structure for holding a transaction output.
 */
typedef struct cck_TransactionOutput cck_TransactionOutput;

/**
 * Opaque data structure for holding a logging connection.
 *
 * The logging connection can be used to manually stop logging.
 *
 * Messages that were logged before a connection is created are buffered in a
 * 1MB buffer. Logging can alternatively be permanently disabled by calling
 * @ref cck_logging_disable. Functions changing the logging settings are
 * global and change the settings for all existing cck_LoggingConnection
 * instances.
 */
typedef struct cck_LoggingConnection cck_LoggingConnection;

/**
 * Opaque data structure for holding the chain parameters.
 *
 * These are eventually placed into a kernel context through the kernel context
 * options. The parameters describe the properties of a chain, and may be
 * instantiated for either mainnet, testnet, signet, or regtest.
 */
typedef struct cck_ChainParameters cck_ChainParameters;

/**
 * Opaque data structure for holding options for creating a new kernel context.
 *
 * Once a kernel context has been created from these options, they may be
 * destroyed. The options hold the notification and validation interface
 * callbacks as well as the selected chain type until they are passed to the
 * context. If no options are configured, the context will be instantiated with
 * no callbacks and for mainnet. Their content and scope can be expanded over
 * time.
 */
typedef struct cck_ContextOptions cck_ContextOptions;

/**
 * Opaque data structure for holding a kernel context.
 *
 * The kernel context is used to initialize internal state and hold the chain
 * parameters and callbacks for handling error and validation events. Once
 * other validation objects are instantiated from it, the context is kept in
 * memory for the duration of their lifetimes.
 *
 * The processing of validation events is done through an internal task runner
 * owned by the context. It passes events through the registered validation
 * interface callbacks.
 *
 * A constructed context can be safely used from multiple threads.
 */
typedef struct cck_Context cck_Context;

/**
 * Opaque data structure for holding a block tree entry.
 *
 * This is a pointer to an element in the block index currently in memory of
 * the chainstate manager. It is valid for the lifetime of the chainstate
 * manager it was retrieved from. The entry is part of a tree-like structure
 * that is maintained internally. Every entry, besides the genesis, points to a
 * single parent. Multiple entries may share a parent, thus forming a tree.
 * Each entry corresponds to a single block and may be used to retrieve its
 * data and validation status.
 */
typedef struct cck_BlockTreeEntry cck_BlockTreeEntry;

/**
 * Opaque data structure for holding options for creating a new chainstate
 * manager.
 *
 * The chainstate manager options are used to set some parameters for the
 * chainstate manager.
 */
typedef struct cck_ChainstateManagerOptions cck_ChainstateManagerOptions;

/**
 * Opaque data structure for holding a chainstate manager.
 *
 * The chainstate manager is the central object for doing validation tasks as
 * well as retrieving data from the chain. Internally it is a complex data
 * structure with diverse functionality.
 *
 * Its functionality will be more and more exposed in the future.
 */
typedef struct cck_ChainstateManager cck_ChainstateManager;

/**
 * Opaque data structure for holding a block.
 */
typedef struct cck_Block cck_Block;

/**
 * Opaque data structure for holding the state of a block during validation.
 *
 * Contains information indicating whether validation was successful, and if not
 * which step during block validation failed.
 */
typedef struct cck_BlockValidationState cck_BlockValidationState;

/**
 * Opaque data structure for holding the Consensus Params.
 */
typedef struct cck_ConsensusParams cck_ConsensusParams;

/**
 * Opaque data structure for holding the currently known best-chain associated
 * with a chainstate.
 */
typedef struct cck_Chain cck_Chain;

/**
 * Opaque data structure for holding the state of a transaction during validation.
 *
 * Contains information indicating whether validation was successful, and if not
 * which step during transaction validation failed.
 */
typedef struct cck_TxValidationState cck_TxValidationState;

/**
 * Opaque data structure for holding a block's spent outputs.
 *
 * Contains all the previous outputs consumed by all transactions in a specific
 * block. Internally it holds a nested vector. The top level vector has an
 * entry for each transaction in a block (in order of the actual transactions
 * of the block and without the coinbase transaction). This is exposed through
 * @ref cck_TransactionSpentOutputs. Each cck_TransactionSpentOutputs is in
 * turn a vector of all the previous outputs of a transaction (in order of
 * their corresponding inputs).
 */
typedef struct cck_BlockSpentOutputs cck_BlockSpentOutputs;

/**
 * Opaque data structure for holding a transaction's spent outputs.
 *
 * Holds the coins consumed by a certain transaction. Retrieved through the
 * @ref cck_BlockSpentOutputs. The coins are in the same order as the
 * transaction's inputs consuming them.
 */
typedef struct cck_TransactionSpentOutputs cck_TransactionSpentOutputs;

/**
 * Opaque data structure for holding a coin.
 *
 * Holds information on the @ref cck_TransactionOutput held within,
 * including the height it was spent at and whether it is a coinbase output.
 */
typedef struct cck_Coin cck_Coin;

/**
 * Opaque data structure for holding a block hash.
 *
 * This is a type-safe identifier for a block.
 */
typedef struct cck_BlockHash cck_BlockHash;

/**
 * Opaque data structure for holding a transaction input.
 *
 * Holds information on the @ref cck_TransactionOutPoint, @ref cck_WitnessStack and script_sig held within.
 */
typedef struct cck_TransactionInput cck_TransactionInput;

/**
 * Opaque data structure for holding a witness stack.
 *
 * Holds a sequence of witness stack items.
 */
typedef struct cck_WitnessStack cck_WitnessStack;

/**
 * Opaque data structure for holding a transaction out point.
 *
 * Holds the txid and output index it is pointing to.
 */
typedef struct cck_TransactionOutPoint cck_TransactionOutPoint;

/**
 * Opaque data structure for holding precomputed transaction data.
 *
 * Reusable when verifying multiple inputs of the same transaction.
 * This avoids recomputing transaction hashes for each input.
 *
 * Required when verifying a taproot input.
 */
typedef struct cck_PrecomputedTransactionData cck_PrecomputedTransactionData;

/**
 * Opaque data structure for holding a cck_Txid.
 *
 * This is a type-safe identifier for a transaction.
 */
typedef struct cck_Txid cck_Txid;

/**
 * Opaque data structure for holding a cck_BlockHeader.
 */
typedef struct cck_BlockHeader cck_BlockHeader;

/** Current sync state passed to tip changed callbacks. */
typedef uint8_t cck_SynchronizationState;
#define cck_SynchronizationState_INIT_REINDEX ((cck_SynchronizationState)(0))
#define cck_SynchronizationState_INIT_DOWNLOAD ((cck_SynchronizationState)(1))
#define cck_SynchronizationState_POST_INIT ((cck_SynchronizationState)(2))

/** Possible warning types issued by validation. */
typedef uint8_t cck_Warning;
#define cck_Warning_UNKNOWN_NEW_RULES_ACTIVATED ((cck_Warning)(0))
#define cck_Warning_LARGE_WORK_INVALID_CHAIN ((cck_Warning)(1))

/** Callback function types */

/**
 * Function signature for the global logging callback. All ConnectCoin kernel
 * internal logs will pass through this callback.
 */
typedef void (*cck_LogCallback)(void* user_data, const char* message, size_t message_len);

/**
 * Function signature for freeing user data.
 */
typedef void (*cck_DestroyCallback)(void* user_data);

/**
 * Function signatures for the kernel notifications.
 */
typedef void (*cck_NotifyBlockTip)(void* user_data, cck_SynchronizationState state, const cck_BlockTreeEntry* entry, double verification_progress);
typedef void (*cck_NotifyHeaderTip)(void* user_data, cck_SynchronizationState state, int64_t height, int64_t timestamp, int presync);
typedef void (*cck_NotifyProgress)(void* user_data, const char* title, size_t title_len, int progress_percent, int resume_possible);
typedef void (*cck_NotifyWarningSet)(void* user_data, cck_Warning warning, const char* message, size_t message_len);
typedef void (*cck_NotifyWarningUnset)(void* user_data, cck_Warning warning);
typedef void (*cck_NotifyFlushError)(void* user_data, const char* message, size_t message_len);
typedef void (*cck_NotifyFatalError)(void* user_data, const char* message, size_t message_len);

/**
 * Function signatures for the validation interface.
 */
typedef void (*cck_ValidationInterfaceBlockChecked)(void* user_data, cck_Block* block, const cck_BlockValidationState* state);
typedef void (*cck_ValidationInterfacePoWValidBlock)(void* user_data, cck_Block* block, const cck_BlockTreeEntry* entry);
typedef void (*cck_ValidationInterfaceBlockConnected)(void* user_data, cck_Block* block, const cck_BlockTreeEntry* entry);
typedef void (*cck_ValidationInterfaceBlockDisconnected)(void* user_data, cck_Block* block, const cck_BlockTreeEntry* entry);

/**
 * Function signature for serializing data.
 *
 * Returns 0 to indicate success.
 */
typedef int (*cck_WriteBytes)(const void* bytes, size_t size, void* userdata);

/**
 * Whether a validated data structure is valid, invalid, or an error was
 * encountered during processing.
 */
typedef uint8_t cck_ValidationMode;
#define cck_ValidationMode_VALID ((cck_ValidationMode)(0))
#define cck_ValidationMode_INVALID ((cck_ValidationMode)(1))
#define cck_ValidationMode_INTERNAL_ERROR ((cck_ValidationMode)(2))

/**
 * A granular "reason" why a block was invalid.
 */
typedef uint32_t cck_BlockValidationResult;
#define cck_BlockValidationResult_UNSET ((cck_BlockValidationResult)(0))           //!< initial value. Block has not yet been rejected
#define cck_BlockValidationResult_CONSENSUS ((cck_BlockValidationResult)(1))       //!< invalid by consensus rules (excluding any below reasons)
#define cck_BlockValidationResult_CACHED_INVALID ((cck_BlockValidationResult)(2))  //!< this block was cached as being invalid and we didn't store the reason why
#define cck_BlockValidationResult_INVALID_HEADER ((cck_BlockValidationResult)(3))  //!< invalid proof of work or time too old
#define cck_BlockValidationResult_MUTATED ((cck_BlockValidationResult)(4))         //!< the block's data didn't match the data committed to by the PoW
#define cck_BlockValidationResult_MISSING_PREV ((cck_BlockValidationResult)(5))    //!< We don't have the previous block the checked one is built on
#define cck_BlockValidationResult_INVALID_PREV ((cck_BlockValidationResult)(6))    //!< A block this one builds on is invalid
#define cck_BlockValidationResult_TIME_FUTURE ((cck_BlockValidationResult)(7))     //!< block timestamp was > 2 hours in the future (or our clock is bad)
#define cck_BlockValidationResult_HEADER_LOW_WORK ((cck_BlockValidationResult)(8)) //!< the block header may be on a too-little-work chain

/**
 * Indicates the reason why a transaction failed validation. The subset of
 * values reachable depends on which validation function was used.
 */
typedef uint32_t cck_TxValidationResult;
#define cck_TxValidationResult_UNSET               ((cck_TxValidationResult)(0))  //!< initial value. Tx has not yet been rejected
#define cck_TxValidationResult_CONSENSUS           ((cck_TxValidationResult)(1))  //!< invalid by consensus rules
#define cck_TxValidationResult_INPUTS_NOT_STANDARD ((cck_TxValidationResult)(2))  //!< inputs (covered by txid) failed policy rules
#define cck_TxValidationResult_NOT_STANDARD        ((cck_TxValidationResult)(3))  //!< otherwise didn't meet local policy rules
#define cck_TxValidationResult_MISSING_INPUTS      ((cck_TxValidationResult)(4))  //!< transaction was missing some of its inputs
#define cck_TxValidationResult_PREMATURE_SPEND     ((cck_TxValidationResult)(5))  //!< transaction spends a coinbase too early, or violates locktime/sequence locks
#define cck_TxValidationResult_WITNESS_MUTATED     ((cck_TxValidationResult)(6))  //!< witness may have been malleated or is prior to SegWit activation
#define cck_TxValidationResult_WITNESS_STRIPPED    ((cck_TxValidationResult)(7))  //!< transaction is missing a witness
#define cck_TxValidationResult_CONFLICT            ((cck_TxValidationResult)(8))  //!< tx already in mempool or conflicts with a tx in the chain
#define cck_TxValidationResult_MEMPOOL_POLICY      ((cck_TxValidationResult)(9))  //!< violated mempool's fee/size/descendant/RBF/etc limits
#define cck_TxValidationResult_NO_MEMPOOL          ((cck_TxValidationResult)(10)) //!< this node does not have a mempool so can't validate the transaction
#define cck_TxValidationResult_RECONSIDERABLE      ((cck_TxValidationResult)(11)) //!< fails some policy, but might be acceptable if submitted in a (different) package
#define cck_TxValidationResult_UNKNOWN             ((cck_TxValidationResult)(12)) //!< transaction was not validated because package failed

/**
 * Holds the validation interface callbacks. The user data pointer may be used
 * to point to user-defined structures to make processing the validation
 * callbacks easier. Note that these callbacks block any further validation
 * execution when they are called.
 */
typedef struct {
    void* user_data;                                              //!< Holds a user-defined opaque structure that is passed to the validation
                                                                  //!< interface callbacks. If user_data_destroy is also defined ownership of the
                                                                  //!< user_data is passed to the created context options and subsequently context.
    cck_DestroyCallback user_data_destroy;                       //!< Frees the provided user data structure.
    cck_ValidationInterfaceBlockChecked block_checked;           //!< Called when a new block has been fully validated. Contains the
                                                                  //!< result of its validation.
    cck_ValidationInterfacePoWValidBlock pow_valid_block;        //!< Called when a new block extends the header chain and has a valid transaction
                                                                  //!< and segwit merkle root.
    cck_ValidationInterfaceBlockConnected block_connected;       //!< Called when a block is valid and has now been connected to the best chain.
    cck_ValidationInterfaceBlockDisconnected block_disconnected; //!< Called during a re-org when a block has been removed from the best chain.
} cck_ValidationInterfaceCallbacks;

/**
 * A struct for holding the kernel notification callbacks. The user data
 * pointer may be used to point to user-defined structures to make processing
 * the notifications easier.
 *
 * If user_data_destroy is provided, the kernel will automatically call this
 * callback to clean up user_data when the notification interface object is destroyed.
 * If user_data_destroy is NULL, it is the user's responsibility to ensure that
 * the user_data outlives the kernel objects. Notifications can
 * occur even as kernel objects are deleted, so care has to be taken to ensure
 * safe unwinding.
 */
typedef struct {
    void* user_data;                        //!< Holds a user-defined opaque structure that is passed to the notification callbacks.
                                            //!< If user_data_destroy is also defined ownership of the user_data is passed to the
                                            //!< created context options and subsequently context.
    cck_DestroyCallback user_data_destroy; //!< Frees the provided user data structure.
    cck_NotifyBlockTip block_tip;          //!< The chain's tip was updated to the provided block entry.
    cck_NotifyHeaderTip header_tip;        //!< A new best block header was added.
    cck_NotifyProgress progress;           //!< Reports on current block synchronization progress.
    cck_NotifyWarningSet warning_set;      //!< A warning issued by the kernel library during validation.
    cck_NotifyWarningUnset warning_unset;  //!< A previous condition leading to the issuance of a warning is no longer given.
    cck_NotifyFlushError flush_error;      //!< An error encountered when flushing data to disk.
    cck_NotifyFatalError fatal_error;      //!< An unrecoverable system error encountered by the library.
} cck_NotificationInterfaceCallbacks;

/**
 * A collection of logging categories that may be encountered by kernel code.
 */
typedef uint8_t cck_LogCategory;
#define cck_LogCategory_ALL ((cck_LogCategory)(0))
#define cck_LogCategory_BENCH ((cck_LogCategory)(1))
#define cck_LogCategory_BLOCKSTORAGE ((cck_LogCategory)(2))
#define cck_LogCategory_COINDB ((cck_LogCategory)(3))
#define cck_LogCategory_LEVELDB ((cck_LogCategory)(4))
#define cck_LogCategory_MEMPOOL ((cck_LogCategory)(5))
#define cck_LogCategory_PRUNE ((cck_LogCategory)(6))
#define cck_LogCategory_RAND ((cck_LogCategory)(7))
#define cck_LogCategory_REINDEX ((cck_LogCategory)(8))
#define cck_LogCategory_VALIDATION ((cck_LogCategory)(9))
#define cck_LogCategory_KERNEL ((cck_LogCategory)(10))

/**
 * The level at which logs should be produced.
 */
typedef uint8_t cck_LogLevel;
#define cck_LogLevel_TRACE ((cck_LogLevel)(0))
#define cck_LogLevel_DEBUG ((cck_LogLevel)(1))
#define cck_LogLevel_INFO ((cck_LogLevel)(2))

/**
 * Options controlling the format of log messages.
 *
 * Set fields as non-zero to indicate true.
 */
typedef struct {
    int log_timestamps;               //!< Prepend a timestamp to log messages.
    int log_time_micros;              //!< Log timestamps in microsecond precision.
    int log_threadnames;              //!< Prepend the name of the thread to log messages.
    int log_sourcelocations;          //!< Prepend the source location to log messages.
    int always_print_category_levels; //!< Prepend the log category and level to log messages.
} cck_LoggingOptions;

/**
 * A collection of status codes that may be issued by the script verify function.
 */
typedef uint8_t cck_ScriptVerifyStatus;
#define cck_ScriptVerifyStatus_OK ((cck_ScriptVerifyStatus)(0))
#define cck_ScriptVerifyStatus_ERROR_INVALID_FLAGS_COMBINATION ((cck_ScriptVerifyStatus)(1)) //!< The flags were combined in an invalid way.
#define cck_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED ((cck_ScriptVerifyStatus)(2))    //!< The taproot flag was set, so valid spent_outputs have to be provided.

/**
 * Script verification flags that may be composed with each other.
 */
typedef uint32_t cck_ScriptVerificationFlags;
#define cck_ScriptVerificationFlags_NONE ((cck_ScriptVerificationFlags)(0))
#define cck_ScriptVerificationFlags_P2SH ((cck_ScriptVerificationFlags)(1U << 0))                 //!< evaluate P2SH (BIP16) subscripts
#define cck_ScriptVerificationFlags_DERSIG ((cck_ScriptVerificationFlags)(1U << 2))               //!< enforce strict DER (BIP66) compliance
#define cck_ScriptVerificationFlags_NULLDUMMY ((cck_ScriptVerificationFlags)(1U << 4))            //!< enforce NULLDUMMY (BIP147)
#define cck_ScriptVerificationFlags_CHECKLOCKTIMEVERIFY ((cck_ScriptVerificationFlags)(1U << 9))  //!< enable CHECKLOCKTIMEVERIFY (BIP65)
#define cck_ScriptVerificationFlags_CHECKSEQUENCEVERIFY ((cck_ScriptVerificationFlags)(1U << 10)) //!< enable CHECKSEQUENCEVERIFY (BIP112)
#define cck_ScriptVerificationFlags_WITNESS ((cck_ScriptVerificationFlags)(1U << 11))             //!< enable WITNESS (BIP141)
#define cck_ScriptVerificationFlags_TAPROOT ((cck_ScriptVerificationFlags)(1U << 17))             //!< enable TAPROOT (BIPs 341 & 342)
#define cck_ScriptVerificationFlags_ALL ((cck_ScriptVerificationFlags)(cck_ScriptVerificationFlags_P2SH |                \
                                                                         cck_ScriptVerificationFlags_DERSIG |              \
                                                                         cck_ScriptVerificationFlags_NULLDUMMY |           \
                                                                         cck_ScriptVerificationFlags_CHECKLOCKTIMEVERIFY | \
                                                                         cck_ScriptVerificationFlags_CHECKSEQUENCEVERIFY | \
                                                                         cck_ScriptVerificationFlags_WITNESS |             \
                                                                         cck_ScriptVerificationFlags_TAPROOT))

typedef uint8_t cck_ChainType;
#define cck_ChainType_MAINNET ((cck_ChainType)(0))
#define cck_ChainType_TESTNET ((cck_ChainType)(1))
#define cck_ChainType_TESTNET_4 ((cck_ChainType)(2))
#define cck_ChainType_SIGNET ((cck_ChainType)(3))
#define cck_ChainType_REGTEST ((cck_ChainType)(4))

/** @name TxValidationState
 *  Introspection for transaction validation state.
 */
///@{

/**
 * Create a new cck_TxValidationState.
 */
CONNECTCOINKERNEL_API cck_TxValidationState* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_tx_validation_state_create();

/**
 * Returns the validation mode from an opaque cck_TxValidationState pointer.
 */
CONNECTCOINKERNEL_API cck_ValidationMode cck_tx_validation_state_get_validation_mode(
    const cck_TxValidationState* state) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Returns the validation result from an opaque cck_TxValidationState pointer.
 *
 * cck_transaction_check currently produces only cck_TxValidationResult_UNSET
 * for valid transactions and cck_TxValidationResult_CONSENSUS for invalid
 * ones. Other values remain exposed for forward compatibility with higher-level
 * validation entry points.
 */
CONNECTCOINKERNEL_API cck_TxValidationResult cck_tx_validation_state_get_tx_validation_result(
    const cck_TxValidationState* state) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the cck_TxValidationState.
 */
CONNECTCOINKERNEL_API void cck_tx_validation_state_destroy(cck_TxValidationState* state);

///@}

/** @name Transaction
 * Functions for working with transactions.
 */
///@{

/**
 * @brief Create a new transaction from the serialized data.
 *
 * @param[in] raw_transaction     Serialized transaction.
 * @param[in] raw_transaction_len Length of the serialized transaction.
 * @return                        The transaction, or null on error.
 */
CONNECTCOINKERNEL_API cck_Transaction* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_create(
    const void* raw_transaction, size_t raw_transaction_len);

/**
 * @brief Copy a transaction. Transactions are reference counted, so this just
 * increments the reference count.
 *
 * @param[in] transaction Non-null.
 * @return                The copied transaction.
 */
CONNECTCOINKERNEL_API cck_Transaction* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_copy(
    const cck_Transaction* transaction) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serializes the transaction through the passed in callback to bytes.
 * This is consensus serialization that is also used for the P2P network.
 *
 * @param[in] transaction Non-null.
 * @param[in] writer      Non-null, callback to a write bytes function.
 * @param[in] user_data   Holds a user-defined opaque structure that will be
 *                        passed back through the writer callback.
 * @return                0 on success.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_to_bytes(
    const cck_Transaction* transaction,
    cck_WriteBytes writer,
    void* user_data) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Get the number of outputs of a transaction.
 *
 * @param[in] transaction Non-null.
 * @return                The number of outputs.
 */
CONNECTCOINKERNEL_API size_t cck_transaction_count_outputs(
    const cck_Transaction* transaction) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the transaction outputs at the provided index. The returned
 * transaction output is not owned and depends on the lifetime of the
 * transaction.
 *
 * @param[in] transaction  Non-null.
 * @param[in] output_index The index of the transaction output to be retrieved.
 * @return                 The transaction output
 */
CONNECTCOINKERNEL_API const cck_TransactionOutput* cck_transaction_get_output_at(
    const cck_Transaction* transaction, size_t output_index) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the transaction input at the provided index. The returned
 * transaction input is not owned and depends on the lifetime of the
 * transaction.
 *
 * @param[in] transaction Non-null.
 * @param[in] input_index The index of the transaction input to be retrieved.
 * @return                 The transaction input
 */
CONNECTCOINKERNEL_API const cck_TransactionInput* cck_transaction_get_input_at(
    const cck_Transaction* transaction, size_t input_index) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the number of inputs of a transaction.
 *
 * @param[in] transaction Non-null.
 * @return                The number of inputs.
 */
CONNECTCOINKERNEL_API size_t cck_transaction_count_inputs(
    const cck_Transaction* transaction) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get a transaction's nLockTime value.
 *
 * @param[in] transaction Non-null.
 * @return                The nLockTime value.
 */
CONNECTCOINKERNEL_API uint32_t cck_transaction_get_locktime(
    const cck_Transaction* transaction) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the txid of a transaction. The returned txid is not owned and
 * depends on the lifetime of the transaction.
 *
 * @param[in] transaction Non-null.
 * @return                The txid.
 */
CONNECTCOINKERNEL_API const cck_Txid* cck_transaction_get_txid(
    const cck_Transaction* transaction) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Run context-free consensus validation on a cck_Transaction.
 *
 * Performs basic structural consensus checks (consensus/tx_check::CheckTransaction)
 * without requiring blockchain state.
 *
 * @param[in]  tx               Non-null, the transaction to validate.
 * @param[out] validation_state Non-null, previously created with
 *                              cck_tx_validation_state_create.
 *                              Overwritten in-place with the validation
 *                              result.
 * @return                      1 if valid, 0 if invalid.
 * @note                        Only cck_TxValidationResult_UNSET and
 *                              cck_TxValidationResult_CONSENSUS are
 *                              reachable via this function.
 */
CONNECTCOINKERNEL_API int cck_transaction_check(
    const cck_Transaction* tx,
    cck_TxValidationState* validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the transaction.
 */
CONNECTCOINKERNEL_API void cck_transaction_destroy(cck_Transaction* transaction);

///@}

/** @name PrecomputedTransactionData
 * Functions for working with precomputed transaction data.
 */
///@{

/**
 * @brief Create precomputed transaction data for script verification.
 *
 * @param[in] tx_to             Non-null.
 * @param[in] spent_outputs     Nullable for non-taproot verification. Points to an array of
 *                              outputs spent by the transaction.
 * @param[in] spent_outputs_len Length of the spent_outputs array.
 * @return                      The precomputed data, or null on error.
 */
CONNECTCOINKERNEL_API cck_PrecomputedTransactionData* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_precomputed_transaction_data_create(
    const cck_Transaction* tx_to,
    const cck_TransactionOutput** spent_outputs, size_t spent_outputs_len) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Copy precomputed transaction data.
 *
 * @param[in] precomputed_txdata Non-null.
 * @return                       The copied precomputed transaction data.
 */
CONNECTCOINKERNEL_API cck_PrecomputedTransactionData* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_precomputed_transaction_data_copy(
    const cck_PrecomputedTransactionData* precomputed_txdata) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the precomputed transaction data.
 */
CONNECTCOINKERNEL_API void cck_precomputed_transaction_data_destroy(cck_PrecomputedTransactionData* precomputed_txdata);

///@}

/** @name ScriptPubkey
 * Functions for working with script pubkeys.
 */
///@{

/**
 * @brief Create a script pubkey from serialized data.
 * @param[in] script_pubkey     Serialized script pubkey.
 * @param[in] script_pubkey_len Length of the script pubkey data.
 * @return                      The script pubkey.
 */
CONNECTCOINKERNEL_API cck_ScriptPubkey* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_script_pubkey_create(
    const void* script_pubkey, size_t script_pubkey_len);

/**
 * @brief Copy a script pubkey.
 *
 * @param[in] script_pubkey Non-null.
 * @return                  The copied script pubkey.
 */
CONNECTCOINKERNEL_API cck_ScriptPubkey* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_script_pubkey_copy(
    const cck_ScriptPubkey* script_pubkey) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Verify if the input at input_index of tx_to spends a ConnectCoin
 * type-1 P2PK output. Consensus requires an exact OP_1/32-byte valid x-only
 * compatibility script, an empty scriptSig, and one 64-byte SIGHASH_DEFAULT
 * Schnorr witness. The flags argument is retained for API compatibility, but
 * cannot enable legacy Script forms or relax the type-1 rules. Use
 * cck_script_pubkey_verify_with_time for type-2 PAY_TO_CONNECT spends.
 *
 * @param[in] script_pubkey      Non-null, script pubkey to be spent.
 * @param[in] amount             Amount of the script pubkey's associated output.
 * @param[in] tx_to              Non-null, transaction spending the script_pubkey.
 * @param[in] precomputed_txdata Non-null precomputed data for tx_to containing every spent output.
 * @param[in] input_index        Index of the input in tx_to spending the script_pubkey.
 * @param[in] flags              Bitfield of cck_ScriptVerificationFlags controlling validation constraints.
 * @param[out] status            Nullable, will be set to an error code if the operation fails, or OK otherwise.
 * @return                       1 if the script is valid, 0 otherwise.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_script_pubkey_verify(
    const cck_ScriptPubkey* script_pubkey,
    int64_t amount,
    const cck_Transaction* tx_to,
    const cck_PrecomputedTransactionData* precomputed_txdata,
    unsigned int input_index,
    cck_ScriptVerificationFlags flags,
    cck_ScriptVerifyStatus* status) CONNECTCOINKERNEL_ARG_NONNULL(1, 3);

/**
 * @brief Verify a ConnectCoin typed-output spend. Type 1 uses a Schnorr
 * witness. Type 2 uses a P2C TLS proof and validates certificate time against
 * p2c_validation_time, normally the previous block's median time.
 *
 * @param[in] script_pubkey       Non-null compatibility view of the spent output.
 * @param[in] amount              Amount of the spent output.
 * @param[in] tx_to               Non-null spending transaction.
 * @param[in] precomputed_txdata  Non-null data containing every spent output.
 * @param[in] input_index         Input being verified.
 * @param[in] flags               Verification flags retained for API compatibility.
 * @param[in] p2c_validation_time Previous-block median time for P2C; ignored for P2PK.
 * @param[out] status             Nullable operation status.
 * @return                        1 if the typed-output spend is valid, 0 otherwise.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_script_pubkey_verify_with_time(
    const cck_ScriptPubkey* script_pubkey,
    int64_t amount,
    const cck_Transaction* tx_to,
    const cck_PrecomputedTransactionData* precomputed_txdata,
    unsigned int input_index,
    cck_ScriptVerificationFlags flags,
    int64_t p2c_validation_time,
    cck_ScriptVerifyStatus* status) CONNECTCOINKERNEL_ARG_NONNULL(1, 3);

/**
 * @brief Serializes the script pubkey through the passed in callback to bytes.
 *
 * @param[in] script_pubkey Non-null.
 * @param[in] writer        Non-null, callback to a write bytes function.
 * @param[in] user_data     Holds a user-defined opaque structure that will be
 *                          passed back through the writer callback.
 * @return                  0 on success.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_script_pubkey_to_bytes(
    const cck_ScriptPubkey* script_pubkey,
    cck_WriteBytes writer,
    void* user_data) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the script pubkey.
 */
CONNECTCOINKERNEL_API void cck_script_pubkey_destroy(cck_ScriptPubkey* script_pubkey);

///@}

/** @name TransactionOutput
 * Functions for working with transaction outputs.
 */
///@{

/**
 * @brief Create a transaction output from a script pubkey and an amount.
 *
 * @param[in] script_pubkey Non-null.
 * @param[in] amount        The amount associated with the script pubkey for this output.
 * @return                  The transaction output, or null if script_pubkey is not a valid
 *                          ConnectCoin type-1 P2PK or type-2 PAY_TO_CONNECT compatibility view.
 */
CONNECTCOINKERNEL_API cck_TransactionOutput* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_output_create(
    const cck_ScriptPubkey* script_pubkey,
    int64_t amount) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the script pubkey of the output. The returned
 * script pubkey is not owned and depends on the lifetime of the
 * transaction output.
 *
 * @param[in] transaction_output Non-null.
 * @return                       The script pubkey.
 */
CONNECTCOINKERNEL_API const cck_ScriptPubkey* cck_transaction_output_get_script_pubkey(
    const cck_TransactionOutput* transaction_output) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the amount in the output.
 *
 * @param[in] transaction_output Non-null.
 * @return                       The amount.
 */
CONNECTCOINKERNEL_API int64_t cck_transaction_output_get_amount(
    const cck_TransactionOutput* transaction_output) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 *  @brief Copy a transaction output.
 *
 *  @param[in] transaction_output Non-null.
 *  @return                       The copied transaction output.
 */
CONNECTCOINKERNEL_API cck_TransactionOutput* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_output_copy(
    const cck_TransactionOutput* transaction_output) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the transaction output.
 */
CONNECTCOINKERNEL_API void cck_transaction_output_destroy(cck_TransactionOutput* transaction_output);

///@}

/** @name Logging
 * Logging-related functions.
 */
///@{

/**
 * @brief This disables the global internal logger. No log messages will be
 * buffered internally anymore once this is called and the buffer is cleared.
 * This function should only be called once and is not thread or re-entry safe.
 * Log messages will be buffered until this function is called, or a logging
 * connection is created. This must not be called while a logging connection
 * already exists.
 */
CONNECTCOINKERNEL_API void cck_logging_disable();

/**
 * @brief Set some options for the global internal logger. This changes global
 * settings and will override settings for all existing @ref
 * cck_LoggingConnection instances.
 *
 * @param[in] options Sets formatting options of the log messages.
 */
CONNECTCOINKERNEL_API void cck_logging_set_options(cck_LoggingOptions options);

/**
 * @brief Set the log level of the global internal logger. This does not
 * enable the selected categories. Use @ref cck_logging_enable_category to
 * start logging from a specific, or all categories. This changes a global
 * setting and will override settings for all existing
 * @ref cck_LoggingConnection instances.
 *
 * @param[in] category If cck_LogCategory_ALL is chosen, sets both the global fallback log level
 *                     used by all categories that don't have a specific level set, and also
 *                     sets the log level for messages logged with the cck_LogCategory_ALL category itself.
 *                     For any other category, sets a category-specific log level that overrides
 *                     the global fallback for that category only.

 * @param[in] level    Log level at which the log category is set.
 */
CONNECTCOINKERNEL_API void cck_logging_set_level_category(cck_LogCategory category, cck_LogLevel level);

/**
 * @brief Enable a specific log category for the global internal logger. This
 * changes a global setting and will override settings for all existing @ref
 * cck_LoggingConnection instances.
 *
 * @param[in] category If cck_LogCategory_ALL is chosen, all categories will be enabled.
 */
CONNECTCOINKERNEL_API void cck_logging_enable_category(cck_LogCategory category);

/**
 * @brief Disable a specific log category for the global internal logger. This
 * changes a global setting and will override settings for all existing @ref
 * cck_LoggingConnection instances.
 *
 * @param[in] category If cck_LogCategory_ALL is chosen, all categories will be disabled.
 */
CONNECTCOINKERNEL_API void cck_logging_disable_category(cck_LogCategory category);

/**
 * @brief Start logging messages through the provided callback. Log messages
 * produced before this function is first called are buffered and on calling this
 * function are logged immediately.
 *
 * @param[in] log_callback               Non-null, function through which messages will be logged.
 * @param[in] user_data                  Nullable, holds a user-defined opaque structure. Is passed back
 *                                       to the user through the callback. If the user_data_destroy_callback
 *                                       is also defined it is assumed that ownership of the user_data is passed
 *                                       to the created logging connection.
 * @param[in] user_data_destroy_callback Nullable, function for freeing the user data.
 * @return                               A new kernel logging connection, or null on error.
 */
CONNECTCOINKERNEL_API cck_LoggingConnection* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_logging_connection_create(
    cck_LogCallback log_callback,
    void* user_data,
    cck_DestroyCallback user_data_destroy_callback) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Stop logging and destroy the logging connection.
 */
CONNECTCOINKERNEL_API void cck_logging_connection_destroy(cck_LoggingConnection* logging_connection);

///@}

/** @name ChainParameters
 * Functions for working with chain parameters.
 */
///@{

/**
 * @brief Creates a chain parameters struct with default parameters based on the
 * passed in chain type.
 *
 * @param[in] chain_type Controls the chain parameters type created.
 * @return               An allocated chain parameters opaque struct.
 */
CONNECTCOINKERNEL_API cck_ChainParameters* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chain_parameters_create(
    cck_ChainType chain_type);

/**
 * @brief Create a signet chain parameters struct with a user-provided
 * challenge.
 *
 * @param[in] challenge     A trivial truthy signet challenge that needs no
 *                          scriptSig or witness solution. Arbitrary BIP325
 *                          Script challenges are unsupported by typed outputs.
 * @param[in] challenge_len The length of the signet challenge.
 * @return                  An allocated chain parameters opaque struct, or null
 *                          if the challenge is unsupported.
 */
CONNECTCOINKERNEL_API cck_ChainParameters* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chain_parameters_create_signet(
    const void* challenge, size_t challenge_len);

/**
 * Copy the chain parameters.
 */
CONNECTCOINKERNEL_API cck_ChainParameters* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chain_parameters_copy(
    const cck_ChainParameters* chain_parameters) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get cck_ConsensusParams from cck_ChainParameters. The returned
 * cck_ConsensusParams pointer is valid only for the lifetime of the
 * cck_ChainParameters object and must not be destroyed by the caller.
 *
 * @param[in] chain_parameters  Non-null.
 * @return                      The cck_ConsensusParams.
 */
CONNECTCOINKERNEL_API const cck_ConsensusParams* cck_chain_parameters_get_consensus_params(
    const cck_ChainParameters* chain_parameters) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the chain parameters.
 */
CONNECTCOINKERNEL_API void cck_chain_parameters_destroy(cck_ChainParameters* chain_parameters);

///@}

/** @name ContextOptions
 * Functions for working with context options.
 */
///@{

/**
 * Creates an empty context options.
 */
CONNECTCOINKERNEL_API cck_ContextOptions* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_context_options_create();

/**
 * @brief Sets the chain params for the context options. The context created
 * with the options will be configured for these chain parameters.
 *
 * @param[in] context_options  Non-null, previously created by @ref cck_context_options_create.
 * @param[in] chain_parameters Is set to the context options.
 */
CONNECTCOINKERNEL_API void cck_context_options_set_chainparams(
    cck_ContextOptions* context_options,
    const cck_ChainParameters* chain_parameters) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Set the kernel notifications for the context options. The context
 * created with the options will be configured with these notifications.
 *
 * @param[in] context_options Non-null, previously created by @ref cck_context_options_create.
 * @param[in] notifications   Is set to the context options.
 */
CONNECTCOINKERNEL_API void cck_context_options_set_notifications(
    cck_ContextOptions* context_options,
    cck_NotificationInterfaceCallbacks notifications) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Set the validation interface callbacks for the context options. The
 * context created with the options will be configured for these validation
 * interface callbacks. The callbacks will then be triggered from validation
 * events issued by the chainstate manager created from the same context.
 *
 * @param[in] context_options                Non-null, previously created with cck_context_options_create.
 * @param[in] validation_interface_callbacks The callbacks used for passing validation information to the
 *                                           user.
 */
CONNECTCOINKERNEL_API void cck_context_options_set_validation_interface(
    cck_ContextOptions* context_options,
    cck_ValidationInterfaceCallbacks validation_interface_callbacks) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the context options.
 */
CONNECTCOINKERNEL_API void cck_context_options_destroy(cck_ContextOptions* context_options);

///@}

/** @name Context
 * Functions for working with contexts.
 */
///@{

/**
 * @brief Create a new kernel context. If the options have not been previously
 * set, their corresponding fields will be initialized to default values; the
 * context will assume mainnet chain parameters and won't attempt to call the
 * kernel notification callbacks.
 *
 * @param[in] context_options Nullable, created by @ref cck_context_options_create.
 * @return                    The allocated context, or null on error.
 */
CONNECTCOINKERNEL_API cck_Context* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_context_create(
    const cck_ContextOptions* context_options);

/**
 * Copy the context.
 */
CONNECTCOINKERNEL_API cck_Context* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_context_copy(
    const cck_Context* context) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Interrupt can be used to halt long-running validation functions like
 * when reindexing, importing or processing blocks.
 *
 * @param[in] context  Non-null.
 * @return             0 if the interrupt was successful, non-zero otherwise.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_context_interrupt(
    cck_Context* context) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the context.
 */
CONNECTCOINKERNEL_API void cck_context_destroy(cck_Context* context);

///@}

/** @name BlockTreeEntry
 * Functions for working with block tree entries.
 */
///@{

/**
 * @brief Returns the previous block tree entry in the tree, or null if the current
 * block tree entry is the genesis block.
 *
 * @param[in] block_tree_entry Non-null.
 * @return                     The previous block tree entry, or null on error or if the current block tree entry is the genesis block.
 */
CONNECTCOINKERNEL_API const cck_BlockTreeEntry* cck_block_tree_entry_get_previous(
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Return the cck_BlockHeader associated with this entry.
 *
 * @param[in] block_tree_entry Non-null.
 * @return                     cck_BlockHeader.
 */
CONNECTCOINKERNEL_API cck_BlockHeader* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_tree_entry_get_block_header(
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Return the height of a certain block tree entry.
 *
 * @param[in] block_tree_entry Non-null.
 * @return                     The block height.
 */
CONNECTCOINKERNEL_API int32_t cck_block_tree_entry_get_height(
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Return the block hash associated with a block tree entry.
 *
 * @param[in] block_tree_entry Non-null.
 * @return                     The block hash.
 */
CONNECTCOINKERNEL_API const cck_BlockHash* cck_block_tree_entry_get_block_hash(
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Check if two block tree entries are equal. Two block tree entries are equal when they
 * point to the same block.
 *
 * @param[in] entry1 Non-null.
 * @param[in] entry2 Non-null.
 * @return           1 if the block tree entries are equal, 0 otherwise.
 */
CONNECTCOINKERNEL_API int cck_block_tree_entry_equals(
    const cck_BlockTreeEntry* entry1, const cck_BlockTreeEntry* entry2) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Return the ancestor of a cck_BlockTreeEntry at the given height.
 *
 * @param[in] block_tree_entry Non-null.
 * @param[in] height           The height of the requested ancestor.
 * @return                     The ancestor at the given height.
 */
CONNECTCOINKERNEL_API const cck_BlockTreeEntry* cck_block_tree_entry_get_ancestor(
    const cck_BlockTreeEntry* block_tree_entry,
    int32_t height) CONNECTCOINKERNEL_ARG_NONNULL(1);

///@}

/** @name ChainstateManagerOptions
 * Functions for working with chainstate manager options.
 */
///@{

/**
 * @brief Create options for the chainstate manager.
 *
 * @param[in] context          Non-null, the created options and through it the chainstate manager will
 *                             associate with this kernel context for the duration of their lifetimes.
 * @param[in] data_directory   Path string of the directory containing the chainstate data. If the directory
 *                             does not exist yet, it will be created.
 * @param[in] blocks_directory Path string of the directory containing the block data. If the directory
 *                             does not exist yet, it will be created.
 * @return                     The allocated chainstate manager options, or null on error (e.g. if a path is invalid).
 */
CONNECTCOINKERNEL_API cck_ChainstateManagerOptions* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_options_create(
    const cck_Context* context,
    const char* data_directory,
    size_t data_directory_len,
    const char* blocks_directory,
    size_t blocks_directory_len) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Set the number of available worker threads used during validation.
 *
 * @param[in] chainstate_manager_options Non-null, options to be set.
 * @param[in] worker_threads             The number of worker threads that should be spawned in the thread pool
 *                                       used for validation. When set to 0 no parallel verification is done.
 *                                       The value range is clamped internally between 0 and 15.
 */
CONNECTCOINKERNEL_API void cck_chainstate_manager_options_set_worker_threads_num(
    cck_ChainstateManagerOptions* chainstate_manager_options,
    int worker_threads) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Set the total database cache used by the chainstate manager.
 *
 * The total cache is split internally between the block tree database,
 * chainstate database, and in-memory coins cache. If this function is not
 * called, the total cache defaults to 450 MiB.
 *
 * @param[in] chainstate_manager_options Non-null, options to be set.
 * @param[in] database_cache_bytes       The total database cache size in bytes. Values below 4 MiB are rejected.
 *                                       On 32-bit systems, values above 1 GiB are also rejected.
 * @return                               0 if the set was successful, non-zero if the set failed.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_options_set_database_cache_bytes(
    cck_ChainstateManagerOptions* chainstate_manager_options,
    uint64_t database_cache_bytes) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Sets wipe db in the options. In combination with calling
 * @ref cck_chainstate_manager_import_blocks this triggers either a full reindex,
 * or a reindex of just the chainstate database.
 *
 * @param[in] chainstate_manager_options Non-null, created by @ref cck_chainstate_manager_options_create.
 * @param[in] wipe_block_tree_db         Set wipe block tree db. Should only be 1 if wipe_chainstate_db is 1 too.
 * @param[in] wipe_chainstate_db         Set wipe chainstate db.
 * @return                               0 if the set was successful, non-zero if the set failed.
 * @note                                 When a wipe is set, the caller must invoke @ref cck_chainstate_manager_import_blocks
 *                                       on the resulting chainstate manager before using it for anything else.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_options_set_wipe_dbs(
    cck_ChainstateManagerOptions* chainstate_manager_options,
    int wipe_block_tree_db,
    int wipe_chainstate_db) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Sets block tree db in memory in the options.
 *
 * @param[in] chainstate_manager_options   Non-null, created by @ref cck_chainstate_manager_options_create.
 * @param[in] block_tree_db_in_memory      Set block tree db in memory.
 */
CONNECTCOINKERNEL_API void cck_chainstate_manager_options_update_block_tree_db_in_memory(
    cck_ChainstateManagerOptions* chainstate_manager_options,
    int block_tree_db_in_memory) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Sets chainstate db in memory in the options.
 *
 * @param[in] chainstate_manager_options Non-null, created by @ref cck_chainstate_manager_options_create.
 * @param[in] chainstate_db_in_memory    Set chainstate db in memory.
 */
CONNECTCOINKERNEL_API void cck_chainstate_manager_options_update_chainstate_db_in_memory(
    cck_ChainstateManagerOptions* chainstate_manager_options,
    int chainstate_db_in_memory) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the chainstate manager options.
 */
CONNECTCOINKERNEL_API void cck_chainstate_manager_options_destroy(cck_ChainstateManagerOptions* chainstate_manager_options);

///@}

/** @name ChainstateManager
 * Functions for chainstate management.
 */
///@{

/**
 * @brief Create a chainstate manager. This is the main object for many
 * validation tasks as well as for retrieving data from the chain and
 * interacting with its chainstate and indexes.
 *
 * @param[in] chainstate_manager_options Non-null, created by @ref cck_chainstate_manager_options_create.
 * @return                               The allocated chainstate manager, or null on error.
 */
CONNECTCOINKERNEL_API cck_ChainstateManager* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_create(
    const cck_ChainstateManagerOptions* chainstate_manager_options) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the cck_BlockTreeEntry whose associated cck_BlockHeader has the most
 * known cumulative proof of work.
 *
 * @param[in] chainstate_manager Non-null.
 * @return                       The cck_BlockTreeEntry, or null if no block headers have been loaded.
 */
CONNECTCOINKERNEL_API const cck_BlockTreeEntry* cck_chainstate_manager_get_best_entry(
    const cck_ChainstateManager* chainstate_manager) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Processes and validates the provided cck_BlockHeader.
 *
 * @param[in] chainstate_manager        Non-null.
 * @param[in] header                    Non-null cck_BlockHeader to be validated.
 * @return                              The cck_BlockValidationState containing validation result, or null on error.
 */
CONNECTCOINKERNEL_API cck_BlockValidationState* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_process_block_header(
    cck_ChainstateManager* chainstate_manager,
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Triggers the start of a reindex if the wipe options were previously
 * set for the chainstate manager. Can also import an array of existing block
 * files selected by the user.
 *
 * @param[in] chainstate_manager        Non-null.
 * @param[in] block_file_paths_data     Nullable, array of block files described by their full filesystem paths.
 * @param[in] block_file_paths_lens     Nullable, array containing the lengths of each of the paths.
 * @param[in] block_file_paths_data_len Length of the block_file_paths_data and block_file_paths_len arrays.
 * @return                              0 if the import blocks call was completed successfully, non-zero otherwise.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_import_blocks(
    cck_ChainstateManager* chainstate_manager,
    const char** block_file_paths_data, size_t* block_file_paths_lens,
    size_t block_file_paths_data_len) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Process and validate the passed in block with the chainstate
 * manager. Processing first does checks on the block, and if these passed,
 * saves it to disk. It then validates the block against the utxo set. If it is
 * valid, the chain is extended with it. The return value is not indicative of
 * the block's validity. Detailed information on the validity of the block can
 * be retrieved by registering the `block_checked` callback in the validation
 * interface.
 *
 * @param[in] chainstate_manager Non-null.
 * @param[in] block              Non-null, block to be validated.
 *
 * @param[out] new_block         Nullable, will be set to 1 if this block was not processed before. Note that this means it
 *                               might also not be 1 if processing was attempted before, but the block was found invalid
 *                               before its data was persisted.
 * @return                       0 if processing the block was successful. Will also return 0 for valid, but duplicate blocks.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_chainstate_manager_process_block(
    cck_ChainstateManager* chainstate_manager,
    const cck_Block* block,
    int* new_block) CONNECTCOINKERNEL_ARG_NONNULL(1, 2, 3);

/**
 * @brief Returns the best known currently active chain. Its lifetime is
 * dependent on the chainstate manager. It can be thought of as a view on a
 * vector of block tree entries that form the best chain. The returned chain
 * reference always points to the currently active best chain. However, state
 * transitions within the chainstate manager (e.g., processing blocks) will
 * update the chain's contents. Data retrieved from this chain is only
 * consistent up to the point when new data is processed in the chainstate
 * manager. It is the user's responsibility to guard against these
 * inconsistencies.
 *
 * @param[in] chainstate_manager Non-null.
 * @return                       The chain.
 */
CONNECTCOINKERNEL_API const cck_Chain* cck_chainstate_manager_get_active_chain(
    const cck_ChainstateManager* chainstate_manager) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Retrieve a block tree entry by its block hash.
 *
 * @param[in] chainstate_manager Non-null.
 * @param[in] block_hash         Non-null.
 * @return                       The block tree entry of the block with the passed in hash, or null if
 *                               the block hash is not found.
 */
CONNECTCOINKERNEL_API const cck_BlockTreeEntry* cck_chainstate_manager_get_block_tree_entry_by_hash(
    const cck_ChainstateManager* chainstate_manager,
    const cck_BlockHash* block_hash) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the chainstate manager.
 */
CONNECTCOINKERNEL_API void cck_chainstate_manager_destroy(cck_ChainstateManager* chainstate_manager);

///@}

/** @name Block
 * Functions for working with blocks.
 */
///@{

/**
 * @brief Reads the block the passed in block tree entry points to from disk and
 * returns it.
 *
 * @param[in] chainstate_manager Non-null.
 * @param[in] block_tree_entry   Non-null.
 * @return                       The read out block, or null on error.
 */
CONNECTCOINKERNEL_API cck_Block* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_read(
    const cck_ChainstateManager* chainstate_manager,
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Parse a serialized raw block into a new block object.
 *
 * @param[in] raw_block     Serialized block.
 * @param[in] raw_block_len Length of the serialized block.
 * @return                  The allocated block, or null on error.
 */
CONNECTCOINKERNEL_API cck_Block* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_create(
    const void* raw_block, size_t raw_block_len);

/**
 * @brief Copy a block. Blocks are reference counted, so this just increments
 * the reference count.
 *
 * @param[in] block Non-null.
 * @return          The copied block.
 */
CONNECTCOINKERNEL_API cck_Block* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_copy(
    const cck_Block* block) CONNECTCOINKERNEL_ARG_NONNULL(1);

/** Bitflags to control context-free block checks (optional). */
typedef uint32_t cck_BlockCheckFlags;
#define cck_BlockCheckFlags_BASE   ((cck_BlockCheckFlags)0)                                                        //!< run the base context-free block checks only
#define cck_BlockCheckFlags_POW    ((cck_BlockCheckFlags)(1U << 0))                                                //!< run CheckProofOfWork with the network bootstrap RandomX key
#define cck_BlockCheckFlags_MERKLE ((cck_BlockCheckFlags)(1U << 1))                                                //!< verify merkle root (and mutation detection)
#define cck_BlockCheckFlags_ALL    ((cck_BlockCheckFlags)(cck_BlockCheckFlags_POW | cck_BlockCheckFlags_MERKLE)) //!< enable all optional context-free block checks

/**
 * @brief Perform context-free validation checks on a cck_Block.
 *
 * Runs the base context-free block checks (size limits, coinbase structure,
 * transaction checks, and sigop limits) using the supplied
 * cck_ConsensusParams. The proof-of-work and merkle-root checks are optional
 * and can be toggled via @p flags. The context-free POW flag uses the network's
 * bootstrap RandomX key and is therefore appropriate only for blocks in that
 * key epoch. Full chain processing derives later epoch keys from ancestry.
 * Note that this does not include any
 * transaction script, timestamps, order, or other checks that may require more
 * context.
 *
 * @param[in]     block             Non-null, cck_Block to validate.
 * @param[in]     consensus_params  Non-null, cck_ConsensusParams for validation.
 * @param[in]     flags             Bitmask of cck_BlockCheckFlags controlling the
 *                                  optional POW and merkle-root checks. Use
 *                                  cck_BlockCheckFlags_BASE to run only the base
 *                                  checks.
 * @param[out]    validation_state  Non-null, previously created with
 *                                  cck_block_validation_state_create.
 *                                  Overwritten in-place with the validation
 *                                  result.
 * @return                          1 if the cck_Block passed the checks, 0 otherwise.
 */
CONNECTCOINKERNEL_API int cck_block_check(
    const cck_Block* block,
    const cck_ConsensusParams* consensus_params,
    cck_BlockCheckFlags flags,
    cck_BlockValidationState* validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1, 2, 4);

/**
 * @brief Perform block checks with an explicitly derived RandomX epoch key.
 *
 * This context-aware variant is required when the POW flag is used outside
 * the bootstrap epoch. The caller derives @p randomx_key from the appropriate
 * key-block ancestry and supplies the candidate block height. Exceptions from
 * RandomX allocation or execution are captured as validation-state errors and
 * never cross the C ABI boundary.
 *
 * @param[in]     block             Non-null block to validate.
 * @param[in]     consensus_params  Non-null consensus parameters.
 * @param[in]     flags             Checks to perform.
 * @param[in]     randomx_key       Non-null RandomX key (32-byte block-hash value).
 * @param[in]     block_height      Non-negative candidate height.
 * @param[out]    validation_state  Non-null validation result.
 * @return 1 if all requested checks passed, 0 otherwise.
 */
CONNECTCOINKERNEL_API int cck_block_check_with_randomx_key(
    const cck_Block* block,
    const cck_ConsensusParams* consensus_params,
    cck_BlockCheckFlags flags,
    const cck_BlockHash* randomx_key,
    int block_height,
    cck_BlockValidationState* validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1, 2, 4, 6);

/**
 * @brief Count the number of transactions contained in a block.
 *
 * @param[in] block Non-null.
 * @return          The number of transactions in the block.
 */
CONNECTCOINKERNEL_API size_t cck_block_count_transactions(
    const cck_Block* block) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the transaction at the provided index. The returned transaction
 * is not owned and depends on the lifetime of the block.
 *
 * @param[in] block             Non-null.
 * @param[in] transaction_index The index of the transaction to be retrieved.
 * @return                      The transaction.
 */
CONNECTCOINKERNEL_API const cck_Transaction* cck_block_get_transaction_at(
    const cck_Block* block, size_t transaction_index) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the cck_BlockHeader from the block.
 *
 * Creates a new cck_BlockHeader object from the block's header data.
 *
 * @param[in] block Non-null cck_Block
 * @return          cck_BlockHeader.
 */
CONNECTCOINKERNEL_API cck_BlockHeader* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_get_header(
    const cck_Block* block) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Calculate and return the hash of a block.
 *
 * @param[in] block Non-null.
 * @return    The block hash.
 */
CONNECTCOINKERNEL_API cck_BlockHash* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_get_hash(
    const cck_Block* block) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serializes the block through the passed in callback to bytes.
 * This is consensus serialization that is also used for the P2P network.
 *
 * @param[in] block     Non-null.
 * @param[in] writer    Non-null, callback to a write bytes function.
 * @param[in] user_data Holds a user-defined opaque structure that will be
 *                      passed back through the writer callback.
 * @return              0 on success.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_to_bytes(
    const cck_Block* block,
    cck_WriteBytes writer,
    void* user_data) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the block.
 */
CONNECTCOINKERNEL_API void cck_block_destroy(cck_Block* block);

///@}

/** @name BlockValidationState
 * Functions for working with block validation states.
 */
///@{

/**
 * Create a new cck_BlockValidationState.
 */
CONNECTCOINKERNEL_API cck_BlockValidationState* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_validation_state_create();

/**
 * Returns the validation mode from an opaque cck_BlockValidationState pointer.
 */
CONNECTCOINKERNEL_API cck_ValidationMode cck_block_validation_state_get_validation_mode(
    const cck_BlockValidationState* block_validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Returns the validation result from an opaque cck_BlockValidationState pointer.
 */
CONNECTCOINKERNEL_API cck_BlockValidationResult cck_block_validation_state_get_block_validation_result(
    const cck_BlockValidationState* block_validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Copies the cck_BlockValidationState.
 *
 * @param[in] block_validation_state Non-null.
 * @return                           The copied cck_BlockValidationState.
 */
CONNECTCOINKERNEL_API cck_BlockValidationState* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_validation_state_copy(
    const cck_BlockValidationState* block_validation_state) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the cck_BlockValidationState.
 */
CONNECTCOINKERNEL_API void cck_block_validation_state_destroy(
    cck_BlockValidationState* block_validation_state);

///@}

/** @name Chain
 * Functions for working with the chain
 */
///@{

/**
 * @brief Return the height of the tip of the chain.
 *
 * @param[in] chain Non-null.
 * @return          The current height.
 */
CONNECTCOINKERNEL_API int32_t cck_chain_get_height(
    const cck_Chain* chain) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Retrieve a block tree entry by its height in the currently active chain.
 * Once retrieved there is no guarantee that it remains in the active chain.
 *
 * @param[in] chain        Non-null.
 * @param[in] block_height Height in the chain of the to be retrieved block tree entry.
 * @return                 The block tree entry at a certain height in the currently active chain, or null
 *                         if the height is out of bounds.
 */
CONNECTCOINKERNEL_API const cck_BlockTreeEntry* cck_chain_get_by_height(
    const cck_Chain* chain,
    int32_t block_height) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Return true if the passed in chain contains the block tree entry.
 *
 * @param[in] chain            Non-null.
 * @param[in] block_tree_entry Non-null.
 * @return                     1 if the block_tree_entry is in the chain, 0 otherwise.
 *
 */
CONNECTCOINKERNEL_API int cck_chain_contains(
    const cck_Chain* chain,
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

///@}

/** @name BlockSpentOutputs
 * Functions for working with block spent outputs.
 */
///@{

/**
 * @brief Reads the block spent coins data the passed in block tree entry points to from
 * disk and returns it.
 *
 * @param[in] chainstate_manager Non-null.
 * @param[in] block_tree_entry   Non-null.
 * @return                       The read out block spent outputs, or null on error.
 */
CONNECTCOINKERNEL_API cck_BlockSpentOutputs* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_spent_outputs_read(
    const cck_ChainstateManager* chainstate_manager,
    const cck_BlockTreeEntry* block_tree_entry) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Copy a block's spent outputs.
 *
 * @param[in] block_spent_outputs Non-null.
 * @return                        The copied block spent outputs.
 */
CONNECTCOINKERNEL_API cck_BlockSpentOutputs* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_spent_outputs_copy(
    const cck_BlockSpentOutputs* block_spent_outputs) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns the number of transaction spent outputs whose data is contained in
 * block spent outputs.
 *
 * @param[in] block_spent_outputs Non-null.
 * @return                        The number of transaction spent outputs data in the block spent outputs.
 */
CONNECTCOINKERNEL_API size_t cck_block_spent_outputs_count(
    const cck_BlockSpentOutputs* block_spent_outputs) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns a transaction spent outputs contained in the block spent
 * outputs at a certain index. The returned pointer is unowned and only valid
 * for the lifetime of block_spent_outputs.
 *
 * @param[in] block_spent_outputs             Non-null.
 * @param[in] transaction_spent_outputs_index The index of the transaction spent outputs within the block spent outputs.
 * @return                                    A transaction spent outputs pointer.
 */
CONNECTCOINKERNEL_API const cck_TransactionSpentOutputs* cck_block_spent_outputs_get_transaction_spent_outputs_at(
    const cck_BlockSpentOutputs* block_spent_outputs,
    size_t transaction_spent_outputs_index) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the block spent outputs.
 */
CONNECTCOINKERNEL_API void cck_block_spent_outputs_destroy(cck_BlockSpentOutputs* block_spent_outputs);

///@}

/** @name TransactionSpentOutputs
 * Functions for working with the spent coins of a transaction
 */
///@{

/**
 * @brief Copy a transaction's spent outputs.
 *
 * @param[in] transaction_spent_outputs Non-null.
 * @return                              The copied transaction spent outputs.
 */
CONNECTCOINKERNEL_API cck_TransactionSpentOutputs* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_spent_outputs_copy(
    const cck_TransactionSpentOutputs* transaction_spent_outputs) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns the number of previous transaction outputs contained in the
 * transaction spent outputs data.
 *
 * @param[in] transaction_spent_outputs Non-null
 * @return                              The number of spent transaction outputs for the transaction.
 */
CONNECTCOINKERNEL_API size_t cck_transaction_spent_outputs_count(
    const cck_TransactionSpentOutputs* transaction_spent_outputs) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns a coin contained in the transaction spent outputs at a
 * certain index. The returned pointer is unowned and only valid for the
 * lifetime of transaction_spent_outputs.
 *
 * @param[in] transaction_spent_outputs Non-null.
 * @param[in] coin_index                The index of the to be retrieved coin within the
 *                                      transaction spent outputs.
 * @return                              A coin pointer.
 */
CONNECTCOINKERNEL_API const cck_Coin* cck_transaction_spent_outputs_get_coin_at(
    const cck_TransactionSpentOutputs* transaction_spent_outputs,
    size_t coin_index) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the transaction spent outputs.
 */
CONNECTCOINKERNEL_API void cck_transaction_spent_outputs_destroy(cck_TransactionSpentOutputs* transaction_spent_outputs);

///@}

/** @name Transaction Input
 * Functions for working with transaction inputs.
 */
///@{

/**
 * @brief Copy a transaction input.
 *
 * @param[in] transaction_input Non-null.
 * @return                      The copied transaction input.
 */
CONNECTCOINKERNEL_API cck_TransactionInput* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_input_copy(
    const cck_TransactionInput* transaction_input) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the transaction out point. The returned transaction out point is
 * not owned and depends on the lifetime of the transaction.
 *
 * @param[in] transaction_input Non-null.
 * @return                      The transaction out point.
 */
CONNECTCOINKERNEL_API const cck_TransactionOutPoint* cck_transaction_input_get_out_point(
    const cck_TransactionInput* transaction_input) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get a transaction input's nSequence value.
 *
 * @param[in] transaction_input Non-null.
 * @return                      The nSequence value.
 */
CONNECTCOINKERNEL_API uint32_t cck_transaction_input_get_sequence(
    const cck_TransactionInput* transaction_input) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the witness stack of a transaction input. The returned witness
 * stack is not owned and depends on the lifetime of the transaction input.
 *
 * @param[in] transaction_input Non-null.
 * @return                      The witness stack.
 */
CONNECTCOINKERNEL_API const cck_WitnessStack* cck_transaction_input_get_witness_stack(
    const cck_TransactionInput* transaction_input) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serialize the script sig of a transaction input through the passed
 * in callback.
 *
 * @param[in] transaction_input Non-null.
 * @param[in] writer            Non-null, function pointer for writing bytes.
 * @param[in] user_data         Nullable, passed back through the writer callback.
 * @return                      The return value of the writer.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_input_get_script_sig(
    const cck_TransactionInput* transaction_input,
    cck_WriteBytes writer,
    void* user_data) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the transaction input.
 */
CONNECTCOINKERNEL_API void cck_transaction_input_destroy(cck_TransactionInput* transaction_input);

///@}

/** @name Witness Stack
 * Functions for working with witness stacks.
 */
///@{

/**
 * @brief Return the number of items in a witness stack.
 *
 * @param[in] witness_stack Non-null.
 * @return                  The number of witness stack items.
 */
CONNECTCOINKERNEL_API size_t cck_witness_stack_count_items(
    const cck_WitnessStack* witness_stack) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serialize a witness stack item at a given index through the passed in
 * callback.
 *
 * @param[in] witness_stack Non-null.
 * @param[in] index         Index of the item in the witness stack.
 * @param[in] writer        Non-null, function pointer for writing bytes.
 * @param[in] user_data     Nullable, passed back through the writer callback.
 * @return                  The return value of the writer.
 * @pre                    index < cck_witness_stack_count_items(witness_stack)
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_witness_stack_get_item_at(
    const cck_WitnessStack* witness_stack,
    size_t index,
    cck_WriteBytes writer,
    void* user_data) CONNECTCOINKERNEL_ARG_NONNULL(1, 3);

/**
 * @brief Copy a witness stack.
 *
 * @param[in] witness_stack Non-null.
 * @return                  The copied witness stack.
 */
CONNECTCOINKERNEL_API cck_WitnessStack* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_witness_stack_copy(
    const cck_WitnessStack* witness_stack) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the witness stack.
 */
CONNECTCOINKERNEL_API void cck_witness_stack_destroy(cck_WitnessStack* witness_stack);

///@}

/** @name Transaction Out Point
 * Functions for working with transaction out points.
 */
///@{

/**
 * @brief Copy a transaction out point.
 *
 * @param[in] transaction_out_point Non-null.
 * @return                          The copied transaction out point.
 */
CONNECTCOINKERNEL_API cck_TransactionOutPoint* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_transaction_out_point_copy(
    const cck_TransactionOutPoint* transaction_out_point) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the output position from the transaction out point.
 *
 * @param[in] transaction_out_point Non-null.
 * @return                          The output index.
 */
CONNECTCOINKERNEL_API uint32_t cck_transaction_out_point_get_index(
    const cck_TransactionOutPoint* transaction_out_point) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the txid from the transaction out point. The returned txid is
 * not owned and depends on the lifetime of the transaction out point.
 *
 * @param[in] transaction_out_point Non-null.
 * @return                          The txid.
 */
CONNECTCOINKERNEL_API const cck_Txid* cck_transaction_out_point_get_txid(
    const cck_TransactionOutPoint* transaction_out_point) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the transaction out point.
 */
CONNECTCOINKERNEL_API void cck_transaction_out_point_destroy(cck_TransactionOutPoint* transaction_out_point);

///@}

/** @name Txid
 * Functions for working with txids.
 */
///@{

/**
 * @brief Copy a txid.
 *
 * @param[in] txid Non-null.
 * @return         The copied txid.
 */
CONNECTCOINKERNEL_API cck_Txid* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_txid_copy(
    const cck_Txid* txid) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Check if two txids are equal.
 *
 * @param[in] txid1 Non-null.
 * @param[in] txid2 Non-null.
 * @return          0 if the txid is not equal.
 */
CONNECTCOINKERNEL_API int cck_txid_equals(
    const cck_Txid* txid1, const cck_Txid* txid2) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Serializes the txid to bytes.
 *
 * @param[in] txid    Non-null.
 * @param[out] output The serialized txid.
 */
CONNECTCOINKERNEL_API void cck_txid_to_bytes(
    const cck_Txid* txid, unsigned char output[32]) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the txid.
 */
CONNECTCOINKERNEL_API void cck_txid_destroy(cck_Txid* txid);

///@}

/** @name Coin
 * Functions for working with coins.
 */
///@{

/**
 * @brief Copy a coin.
 *
 * @param[in] coin Non-null.
 * @return         The copied coin.
 */
CONNECTCOINKERNEL_API cck_Coin* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_coin_copy(
    const cck_Coin* coin) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns the block height where the transaction that
 * created this coin was included in.
 *
 * @param[in] coin Non-null.
 * @return         The block height of the coin.
 */
CONNECTCOINKERNEL_API uint32_t cck_coin_confirmation_height(
    const cck_Coin* coin) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Returns whether the containing transaction was a coinbase.
 *
 * @param[in] coin Non-null.
 * @return         1 if the coin is a coinbase coin, 0 otherwise.
 */
CONNECTCOINKERNEL_API int cck_coin_is_coinbase(
    const cck_Coin* coin) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Return the transaction output of a coin. The returned pointer is
 * unowned and only valid for the lifetime of the coin.
 *
 * @param[in] coin Non-null.
 * @return         A transaction output pointer.
 */
CONNECTCOINKERNEL_API const cck_TransactionOutput* cck_coin_get_output(
    const cck_Coin* coin) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * Destroy the coin.
 */
CONNECTCOINKERNEL_API void cck_coin_destroy(cck_Coin* coin);

///@}

/** @name BlockHash
 * Functions for working with block hashes.
 */
///@{

/**
 * @brief Create a block hash from its raw data.
 */
CONNECTCOINKERNEL_API cck_BlockHash* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_hash_create(
    const unsigned char block_hash[32]) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Check if two block hashes are equal.
 *
 * @param[in] hash1 Non-null.
 * @param[in] hash2 Non-null.
 * @return          0 if the block hashes are not equal.
 */
CONNECTCOINKERNEL_API int cck_block_hash_equals(
    const cck_BlockHash* hash1, const cck_BlockHash* hash2) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * @brief Copy a block hash.
 *
 * @param[in] block_hash Non-null.
 * @return               The copied block hash.
 */
CONNECTCOINKERNEL_API cck_BlockHash* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_hash_copy(
    const cck_BlockHash* block_hash) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serializes the block hash to bytes.
 *
 * @param[in] block_hash     Non-null.
 * @param[in] output         The serialized block hash.
 */
CONNECTCOINKERNEL_API void cck_block_hash_to_bytes(
    const cck_BlockHash* block_hash, unsigned char output[32]) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the block hash.
 */
CONNECTCOINKERNEL_API void cck_block_hash_destroy(cck_BlockHash* block_hash);

///@}

/**
 * @name Block Header
 * Functions for working with block headers.
 */
///@{

/**
 * @brief Create a cck_BlockHeader from serialized data.
 *
 * @param[in] raw_block_header      Non-null, serialized header data (80 bytes)
 * @param[in] raw_block_header_len  Length of serialized header (must be 80)
 * @return                          cck_BlockHeader, or null on error.
 */
CONNECTCOINKERNEL_API cck_BlockHeader* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_header_create(
    const void* raw_block_header, size_t raw_block_header_len) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Copy a cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader.
 * @return              Copied cck_BlockHeader.
 */
CONNECTCOINKERNEL_API cck_BlockHeader* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_header_copy(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the cck_BlockHash.
 *
 * @param[in] header    Non-null header
 * @return              cck_BlockHash.
 */
CONNECTCOINKERNEL_API cck_BlockHash* CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_header_get_hash(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the previous cck_BlockHash from cck_BlockHeader. The returned hash
 * is unowned and only valid for the lifetime of the cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader
 * @return              Previous cck_BlockHash
 */
CONNECTCOINKERNEL_API const cck_BlockHash* cck_block_header_get_prev_hash(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the timestamp from cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader
 * @return              Block timestamp (Unix epoch seconds)
 */
CONNECTCOINKERNEL_API uint32_t cck_block_header_get_timestamp(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the nBits difficulty target from cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader
 * @return              Difficulty target (compact format)
 */
CONNECTCOINKERNEL_API uint32_t cck_block_header_get_bits(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the version from cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader
 * @return              Block version
 */
CONNECTCOINKERNEL_API int32_t cck_block_header_get_version(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Get the nonce from cck_BlockHeader.
 *
 * @param[in] header    Non-null cck_BlockHeader
 * @return              Nonce
 */
CONNECTCOINKERNEL_API uint32_t cck_block_header_get_nonce(
    const cck_BlockHeader* header) CONNECTCOINKERNEL_ARG_NONNULL(1);

/**
 * @brief Serializes the cck_BlockHeader to bytes.
 * This is consensus serialization that is also used for the P2P network.
 *
 * @param[in] header    Non-null.
 * @param[out] output   The serialized block header (80 bytes).
 * @return              0 on success.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_block_header_to_bytes(
    const cck_BlockHeader* header, unsigned char output[80]) CONNECTCOINKERNEL_ARG_NONNULL(1, 2);

/**
 * Destroy the cck_BlockHeader.
 */
CONNECTCOINKERNEL_API void cck_block_header_destroy(cck_BlockHeader* header);

///@}

/** @name Testing
 * Functions intended for testing purposes only.
 */
///@{

/**
 * @brief Override the current time with a fixed timestamp for testing.
 *
 * Affects all kernel time reads globally. The caller is responsible
 * for gating usage (e.g. restricting to regtest) if desired.
 *
 * The upper bound (4294967295) matches the maximum value of a block header
 * timestamp.
 *
 * @param[in] timestamp Unix epoch seconds, or 0 to restore the system clock.
 * @return              0 on success, non-zero if timestamp is outside the
 *                      valid [0, 4294967295] range.
 */
CONNECTCOINKERNEL_API int CONNECTCOINKERNEL_WARN_UNUSED_RESULT cck_set_mock_time(int64_t timestamp);

///@}

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // CONNECTCOIN_KERNEL_CONNECTCOINKERNEL_H
