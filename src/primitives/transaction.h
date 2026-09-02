// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_PRIMITIVES_TRANSACTION_H
#define CONNECTCOIN_PRIMITIVES_TRANSACTION_H

#include <attributes.h>
#include <consensus/amount.h>
#include <primitives/transaction_identifier.h> // IWYU pragma: export
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <uint256.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint
{
public:
    Txid hash;
    uint32_t n;

    static constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

    COutPoint(): n(NULL_INDEX) { }
    COutPoint(const Txid& hashIn, uint32_t nIn): hash(hashIn), n(nIn) { }

    SERIALIZE_METHODS(COutPoint, obj) { READWRITE(obj.hash, obj.n); }

    void SetNull() { hash.SetNull(); n = NULL_INDEX; }
    bool IsNull() const { return (hash.IsNull() && n == NULL_INDEX); }

    friend bool operator<(const COutPoint& a, const COutPoint& b)
    {
        return std::tie(a.hash, a.n) < std::tie(b.hash, b.n);
    }

    friend bool operator==(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    std::string ToString() const;
};

/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;
    CScriptWitness scriptWitness; //!< Only serialized through CTransaction

    /**
     * Setting nSequence to this value for every input in a transaction
     * disables nLockTime/IsFinalTx().
     * It fails OP_CHECKLOCKTIMEVERIFY/CheckLockTime() for any input that has
     * it set (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static constexpr uint32_t SEQUENCE_FINAL{0xffffffff};
    /**
     * This is the maximum sequence number that enables both nLockTime and
     * OP_CHECKLOCKTIMEVERIFY (BIP 65).
     * It has SEQUENCE_LOCKTIME_DISABLE_FLAG set (BIP 68/112).
     */
    static constexpr uint32_t MAX_SEQUENCE_NONFINAL{SEQUENCE_FINAL - 1};

    // Below flags apply in the context of BIP 68. BIP 68 requires the tx
    // version to be set to 2, or higher.
    /**
     * If this flag is set, CTxIn::nSequence is NOT interpreted as a
     * relative lock-time.
     * It skips SequenceLocks() for any input that has it set (BIP 68).
     * It fails OP_CHECKSEQUENCEVERIFY/CheckSequence() for any input that has
     * it set (BIP 112).
     */
    static constexpr uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG{1U << 31};

    /**
     * If CTxIn::nSequence encodes a relative lock-time and this flag
     * is set, the relative lock-time has units of 512 seconds,
     * otherwise it specifies blocks with a granularity of 1. */
    static constexpr uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG{1 << 22};

    /**
     * If CTxIn::nSequence encodes a relative lock-time, this mask is
     * applied to extract that lock-time from the sequence field. */
    static constexpr uint32_t SEQUENCE_LOCKTIME_MASK{0x0000ffff};

    /**
     * In order to use the same number of bits to encode roughly the
     * same wall-clock duration, and because blocks are naturally
     * limited to occur every 600s on average, the minimum granularity
     * for time-based relative lock-time is fixed at 512 seconds.
     * Converting from CTxIn::nSequence to seconds is performed by
     * multiplying by 512 = 2^9, or equivalently shifting up by
     * 9 bits. */
    static constexpr int SEQUENCE_LOCKTIME_GRANULARITY{9};

    CTxIn()
    {
        nSequence = SEQUENCE_FINAL;
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);
    CTxIn(Txid hashPrevTx, uint32_t nOut, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);

    SERIALIZE_METHODS(CTxIn, obj) { READWRITE(obj.prevout, obj.scriptSig, obj.nSequence); }

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    std::string ToString() const;
};

/** Consensus output types. Type 0 is reserved and never spendable. */
enum class TxOutputType : uint8_t {
    INVALID = 0,
    P2PK = 1,
    PAY_TO_CONNECT = 2,
};

/** Maximum canonical DNS name length, excluding a trailing root label. */
inline constexpr size_t MAX_P2C_DOMAIN_LENGTH{253};

/** Immutable payload for a PAY_TO_DOMAIN output. */
struct PayToDomainOutput
{
    std::string domain;
    uint256 connection_work_target;
    uint32_t root_certificates_version{0};

    friend bool operator==(const PayToDomainOutput&, const PayToDomainOutput&) = default;
};

/**
 * Return whether a domain is in the canonical P2C wire form: lower-case ASCII
 * DNS LDH labels, no trailing dot, labels of 1..63 bytes, and a total of at
 * most 253 bytes. Unicode names must be converted to a lower-case ASCII DNS
 * representation by the transaction creator before consensus serialization.
 */
bool IsCanonicalP2CDomain(std::string_view domain);

/** An output of a transaction.
 *
 * The consensus serialization is amount + one-byte type + type payload. For
 * P2PK the payload is exactly one 32-byte x-only public key. P2C uses a
 * one-byte domain length followed by the canonical domain, a 32-byte work
 * target, and a 32-bit immutable root-certificate bundle version. scriptPubKey is a
 * compatibility view used by wallet, descriptor, PSBT, kernel, and RPC code;
 * it is not serialized for valid outputs and is never executed by consensus.
 */
class CTxOut
{
private:
    uint8_t type;
    XOnlyPubKey p2pk_pubkey;

public:
    CAmount nValue;
    CScript scriptPubKey;

    CTxOut()
    {
        SetNull();
    }

    CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn);
    CTxOut(const CAmount& nValueIn, const XOnlyPubKey& pubkeyIn);
    CTxOut(const CAmount& nValueIn, const PayToDomainOutput& p2cIn);

    TxOutputType GetType() const;
    std::optional<XOnlyPubKey> GetP2PKPubKey() const;
    std::optional<PayToDomainOutput> GetPayToDomain() const;
    void SetScriptPubKey(CScript scriptPubKeyIn);
    void SetP2PK(const XOnlyPubKey& pubkeyIn);
    void SetPayToDomain(const PayToDomainOutput& p2cIn);

    template<typename Stream>
    void SerializePayload(Stream& s) const
    {
        ::Serialize(s, type);
        switch (type) {
        case static_cast<uint8_t>(TxOutputType::P2PK):
            if (const auto pubkey{GetP2PKPubKey()}) {
                ::Serialize(s, *pubkey);
            } else {
                throw std::ios_base::failure("Inconsistent P2PK transaction output");
            }
            break;
        case static_cast<uint8_t>(TxOutputType::PAY_TO_CONNECT):
            if (const auto p2c{GetPayToDomain()}) {
                ::Serialize(s, static_cast<uint8_t>(p2c->domain.size()));
                s.write(MakeByteSpan(p2c->domain));
                ::Serialize(s, p2c->connection_work_target);
                ::Serialize(s, p2c->root_certificates_version);
            } else {
                throw std::ios_base::failure("Inconsistent PAY_TO_CONNECT transaction output");
            }
            break;
        case static_cast<uint8_t>(TxOutputType::INVALID):
            // Type 0 has no payload and is rejected by consensus. Keeping it
            // parseable lets null objects and malformed transactions be
            // handled without ever putting Script back on the wire.
            break;
        default:
            throw std::ios_base::failure("Unknown transaction output type");
        }
    }

    template<typename Stream>
    void UnserializePayload(Stream& s)
    {
        uint8_t encoded_type{0};
        ::Unserialize(s, encoded_type);
        type = encoded_type;
        switch (encoded_type) {
        case static_cast<uint8_t>(TxOutputType::P2PK):
            ::Unserialize(s, p2pk_pubkey);
            if (!p2pk_pubkey.IsFullyValid()) {
                throw std::ios_base::failure("Invalid P2PK x-only public key");
            }
            SetP2PK(p2pk_pubkey);
            break;
        case static_cast<uint8_t>(TxOutputType::PAY_TO_CONNECT): {
            uint8_t domain_size{0};
            ::Unserialize(s, domain_size);
            std::string domain(domain_size, '\0');
            if (domain_size != 0) s.read(MakeWritableByteSpan(domain));
            if (!IsCanonicalP2CDomain(domain)) throw std::ios_base::failure("Invalid PAY_TO_CONNECT domain");
            PayToDomainOutput p2c{
                .domain = std::move(domain),
                .connection_work_target = {},
                .root_certificates_version = 0,
            };
            ::Unserialize(s, p2c.connection_work_target);
            ::Unserialize(s, p2c.root_certificates_version);
            if (p2c.root_certificates_version == 0) {
                throw std::ios_base::failure("Invalid PAY_TO_CONNECT root certificate version");
            }
            SetPayToDomain(p2c);
            break;
        }
        case static_cast<uint8_t>(TxOutputType::INVALID):
            p2pk_pubkey = {};
            scriptPubKey.clear();
            break;
        default:
            throw std::ios_base::failure("Unknown transaction output type");
        }
    }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, nValue);
        SerializePayload(s);
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, nValue);
        UnserializePayload(s);
    }

    void SetNull()
    {
        nValue = -1;
        type = static_cast<uint8_t>(TxOutputType::INVALID);
        p2pk_pubkey = {};
        scriptPubKey.clear();
    }

    bool IsNull() const
    {
        return (nValue == -1);
    }

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue == b.nValue && a.GetType() == b.GetType() &&
                a.GetP2PKPubKey() == b.GetP2PKPubKey() &&
                a.GetPayToDomain() == b.GetPayToDomain());
    }

    std::string ToString() const;
};

struct CMutableTransaction;

struct TransactionSerParams {
    const bool allow_witness;
    SER_PARAMS_OPFUNC
};
inline constexpr TransactionSerParams TX_WITH_WITNESS{.allow_witness = true};
inline constexpr TransactionSerParams TX_NO_WITNESS{.allow_witness = false};

/**
 * Basic transaction serialization format:
 * - uint32_t version
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - uint32_t nLockTime
 *
 * Extended transaction serialization format:
 * - uint32_t version
 * - unsigned char dummy = 0x00
 * - unsigned char flags (!= 0)
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - if (flags & 1):
 *   - CScriptWitness scriptWitness; (deserialized into CTxIn)
 * - uint32_t nLockTime
 */
template<typename Stream, typename TxType>
void UnserializeTransaction(TxType& tx, Stream& s, const TransactionSerParams& params)
{
    const bool fAllowWitness = params.allow_witness;

    s >> tx.version;
    unsigned char flags = 0;
    tx.vin.clear();
    tx.vout.clear();
    /* Try to read the vin. In case the dummy is there, this will be read as an empty vector. */
    s >> tx.vin;
    if (tx.vin.size() == 0 && fAllowWitness) {
        /* We read a dummy or an empty vin. */
        s >> flags;
        if (flags != 0) {
            s >> tx.vin;
            s >> tx.vout;
        }
    } else {
        /* We read a non-empty vin. Assume a normal vout follows. */
        s >> tx.vout;
    }
    if ((flags & 1) && fAllowWitness) {
        /* The witness flag is present, and we support witnesses. */
        flags ^= 1;
        for (size_t i = 0; i < tx.vin.size(); i++) {
            s >> tx.vin[i].scriptWitness.stack;
        }
        if (!tx.HasWitness()) {
            /* It's illegal to encode witnesses when all witness stacks are empty. */
            throw std::ios_base::failure("Superfluous witness record");
        }
    }
    if (flags) {
        /* Unknown flag in the serialization */
        throw std::ios_base::failure("Unknown transaction optional data");
    }
    s >> tx.nLockTime;
}

template<typename Stream, typename TxType>
void SerializeTransaction(const TxType& tx, Stream& s, const TransactionSerParams& params)
{
    const bool fAllowWitness = params.allow_witness;

    s << tx.version;
    unsigned char flags = 0;
    // Consistency check
    if (fAllowWitness) {
        /* Check whether witnesses need to be serialized. */
        if (tx.HasWitness()) {
            flags |= 1;
        }
    }
    if (flags) {
        /* Use extended format in case witnesses are to be serialized. */
        std::vector<CTxIn> vinDummy;
        s << vinDummy;
        s << flags;
    }
    s << tx.vin;
    s << tx.vout;
    if (flags & 1) {
        for (size_t i = 0; i < tx.vin.size(); i++) {
            s << tx.vin[i].scriptWitness.stack;
        }
    }
    s << tx.nLockTime;
}

template<typename TxType>
inline CAmount CalculateOutputValue(const TxType& tx)
{
    return std::accumulate(tx.vout.cbegin(), tx.vout.cend(), CAmount{0}, [](CAmount sum, const auto& txout) { return sum + txout.nValue; });
}


/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
public:
    // Default transaction version.
    static constexpr uint32_t CURRENT_VERSION{2};

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, CTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const std::vector<CTxIn> vin;
    const std::vector<CTxOut> vout;
    const uint32_t version;
    const uint32_t nLockTime;

private:
    /** Memory only. */
    const bool m_has_witness;
    const Txid hash;
    const Wtxid m_witness_hash;

    Txid ComputeHash() const;
    Wtxid ComputeWitnessHash() const;

    bool ComputeHasWitness() const;

public:
    /** Convert a CMutableTransaction into a CTransaction. */
    explicit CTransaction(const CMutableTransaction& tx);
    explicit CTransaction(CMutableTransaction&& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    /** This deserializing constructor is provided instead of an Unserialize method.
     *  Unserialize is not possible, since it would require overwriting const fields. */
    template <typename Stream>
    CTransaction(deserialize_type, const TransactionSerParams& params, Stream& s) : CTransaction(CMutableTransaction(deserialize, params, s)) {}
    template <typename Stream>
    CTransaction(deserialize_type, Stream& s) : CTransaction(CMutableTransaction(deserialize, s)) {}

    bool IsNull() const {
        return vin.empty() && vout.empty();
    }

    const Txid& GetHash() const LIFETIMEBOUND { return hash; }
    const Wtxid& GetWitnessHash() const LIFETIMEBOUND { return m_witness_hash; };

    // Return sum of txouts.
    CAmount GetValueOut() const;

    /**
     * Calculate the total transaction size in bytes, including witness data.
     * "Total Size" defined in BIP141 and BIP144.
     * @return Total transaction size in bytes
     */
    unsigned int ComputeTotalSize() const;

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull());
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return a.GetWitnessHash() == b.GetWitnessHash();
    }

    std::string ToString() const;

    bool HasWitness() const { return m_has_witness; }
};

/** A mutable version of CTransaction. */
struct CMutableTransaction
{
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    uint32_t version;
    uint32_t nLockTime;

    explicit CMutableTransaction();
    explicit CMutableTransaction(const CTransaction& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    template <typename Stream>
    inline void Unserialize(Stream& s) {
        UnserializeTransaction(*this, s, s.template GetParams<TransactionSerParams>());
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, const TransactionSerParams& params, Stream& s) {
        UnserializeTransaction(*this, s, params);
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, Stream& s) {
        Unserialize(s);
    }

    /** Compute the hash of this CMutableTransaction. This is computed on the
     * fly, as opposed to GetHash() in CTransaction, which uses a cached result.
     */
    Txid GetHash() const;

    bool HasWitness() const
    {
        for (size_t i = 0; i < vin.size(); i++) {
            if (!vin[i].scriptWitness.IsNull()) {
                return true;
            }
        }
        return false;
    }
};

typedef std::shared_ptr<const CTransaction> CTransactionRef;
template <typename Tx> static inline CTransactionRef MakeTransactionRef(Tx&& txIn) { return std::make_shared<const CTransaction>(std::forward<Tx>(txIn)); }

namespace std {
/** Disable default std::hash for CTransactionRef to prevent accidentally
 *  comparing by pointer. Use CTransactionRefHash or provide a custom
 *  hasher. */
template <>
struct hash<CTransactionRef> {
    hash() = delete;
    // Belt-and-suspenders, already implied by the above.
    size_t operator()(const CTransactionRef&) const = delete;
};
} // namespace std

#endif // CONNECTCOIN_PRIMITIVES_TRANSACTION_H
