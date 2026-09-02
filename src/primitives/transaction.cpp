// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>

#include <consensus/amount.h>
#include <crypto/common.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <primitives/transaction_identifier.h>
#include <script/script.h>
#include <serialize.h>
#include <tinyformat.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <span>
#include <stdexcept>

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10), n);
}

CTxIn::CTxIn(COutPoint prevoutIn, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(Txid hashPrevTx, uint32_t nOut, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != SEQUENCE_FINAL)
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CTxOut::CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    nValue = nValueIn;
    SetScriptPubKey(std::move(scriptPubKeyIn));
}

CTxOut::CTxOut(const CAmount& nValueIn, const XOnlyPubKey& pubkeyIn)
{
    nValue = nValueIn;
    SetP2PK(pubkeyIn);
}

CTxOut::CTxOut(const CAmount& nValueIn, const PayToDomainOutput& p2cIn)
{
    nValue = nValueIn;
    SetPayToDomain(p2cIn);
}

bool IsCanonicalP2CDomain(std::string_view domain)
{
    if (domain.empty() || domain.size() > MAX_P2C_DOMAIN_LENGTH || domain.back() == '.') return false;

    size_t label_size{0};
    for (size_t i{0}; i < domain.size(); ++i) {
        const unsigned char ch{static_cast<unsigned char>(domain[i])};
        if (ch == '.') {
            if (label_size == 0 || label_size > 63 || domain[i - 1] == '-') return false;
            label_size = 0;
            continue;
        }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
        if (label_size == 0 && ch == '-') return false;
        ++label_size;
        if (label_size > 63) return false;
    }
    return label_size != 0 && domain.back() != '-';
}

namespace {

constexpr size_t P2C_VIEW_PREFIX_SIZE{2};

CScript P2CCompatibilityView(std::string_view domain, const uint256& target, uint32_t root_certificates_version)
{
    assert(IsCanonicalP2CDomain(domain));
    CScript view;
    view.reserve(P2C_VIEW_PREFIX_SIZE + domain.size() + target.size() + sizeof(uint32_t));
    view.push_back(OP_2);
    view.push_back(static_cast<unsigned char>(domain.size()));
    view.insert(view.end(), domain.begin(), domain.end());
    view.insert(view.end(), target.begin(), target.end());
    std::array<unsigned char, sizeof(uint32_t)> encoded_version{};
    WriteLE32(encoded_version.data(), root_certificates_version);
    view.insert(view.end(), encoded_version.begin(), encoded_version.end());
    return view;
}

std::optional<std::string_view> P2CDomainFromView(const CScript& view)
{
    if (view.size() < P2C_VIEW_PREFIX_SIZE || view[0] != OP_2) return std::nullopt;
    const size_t domain_size{view[1]};
    if (view.size() != P2C_VIEW_PREFIX_SIZE + domain_size + uint256::size() + sizeof(uint32_t)) return std::nullopt;
    const std::string_view domain{reinterpret_cast<const char*>(view.data() + P2C_VIEW_PREFIX_SIZE), domain_size};
    if (!IsCanonicalP2CDomain(domain)) return std::nullopt;
    return domain;
}

} // namespace

std::optional<XOnlyPubKey> CTxOut::GetP2PKPubKey() const
{
    if (GetType() != TxOutputType::P2PK || !p2pk_pubkey.IsFullyValid()) return std::nullopt;
    const CScript expected{CScript{} << OP_1 << std::vector<unsigned char>{p2pk_pubkey.begin(), p2pk_pubkey.end()}};
    if (scriptPubKey != expected) return std::nullopt;
    return p2pk_pubkey;
}

std::optional<PayToDomainOutput> CTxOut::GetPayToDomain() const
{
    if (GetType() != TxOutputType::PAY_TO_CONNECT) return std::nullopt;
    const auto domain{P2CDomainFromView(scriptPubKey)};
    if (!domain) return std::nullopt;
    const size_t target_pos{P2C_VIEW_PREFIX_SIZE + domain->size()};
    PayToDomainOutput result;
    result.domain = *domain;
    std::copy_n(scriptPubKey.begin() + target_pos, uint256::size(), result.connection_work_target.begin());
    result.root_certificates_version = ReadLE32(scriptPubKey.data() + target_pos + uint256::size());
    if (result.root_certificates_version == 0) return std::nullopt;
    return result;
}

TxOutputType CTxOut::GetType() const
{
    return static_cast<TxOutputType>(type);
}

void CTxOut::SetScriptPubKey(CScript scriptPubKeyIn)
{
    scriptPubKey = std::move(scriptPubKeyIn);
    type = static_cast<uint8_t>(TxOutputType::INVALID);
    p2pk_pubkey = {};
    if (scriptPubKey.size() == 34 && scriptPubKey[0] == OP_1 && scriptPubKey[1] == XOnlyPubKey::size()) {
        XOnlyPubKey pubkey{std::span{scriptPubKey}.subspan(2)};
        if (!pubkey.IsFullyValid()) return;
        type = static_cast<uint8_t>(TxOutputType::P2PK);
        p2pk_pubkey = pubkey;
        return;
    }
    if (scriptPubKey.size() >= P2C_VIEW_PREFIX_SIZE && scriptPubKey[0] == OP_2) {
        type = static_cast<uint8_t>(TxOutputType::PAY_TO_CONNECT);
        if (GetPayToDomain()) return;
    }
    type = static_cast<uint8_t>(TxOutputType::INVALID);
}

void CTxOut::SetP2PK(const XOnlyPubKey& pubkeyIn)
{
    assert(pubkeyIn.IsFullyValid());
    type = static_cast<uint8_t>(TxOutputType::P2PK);
    p2pk_pubkey = pubkeyIn;
    scriptPubKey = CScript{} << OP_1 << std::vector<unsigned char>{pubkeyIn.begin(), pubkeyIn.end()};
}

void CTxOut::SetPayToDomain(const PayToDomainOutput& p2cIn)
{
    assert(IsCanonicalP2CDomain(p2cIn.domain));
    assert(p2cIn.root_certificates_version != 0);
    type = static_cast<uint8_t>(TxOutputType::PAY_TO_CONNECT);
    p2pk_pubkey = {};
    scriptPubKey = P2CCompatibilityView(p2cIn.domain, p2cIn.connection_work_target,
                                        p2cIn.root_certificates_version);
}

std::string CTxOut::ToString() const
{
    std::string payload{"invalid"};
    if (const auto pubkey{GetP2PKPubKey()}) {
        payload = strprintf("pubkey=%s", HexStr(*pubkey));
    } else if (const auto p2c{GetPayToDomain()}) {
        payload = strprintf("domain=%s, target=%s, roots=%u", p2c->domain,
                            p2c->connection_work_target.ToString(), p2c->root_certificates_version);
    }
    return strprintf("CTxOut(nValue=%d.%010d, type=%u, %s)", nValue / COIN, nValue % COIN,
                     static_cast<unsigned>(GetType()), payload);
}

CMutableTransaction::CMutableTransaction() : version{CTransaction::CURRENT_VERSION}, nLockTime{0} {}
CMutableTransaction::CMutableTransaction(const CTransaction& tx) : vin(tx.vin), vout(tx.vout), version{tx.version}, nLockTime{tx.nLockTime} {}

Txid CMutableTransaction::GetHash() const
{
    return Txid::FromUint256((HashWriter{} << TX_NO_WITNESS(*this)).GetHash());
}

bool CTransaction::ComputeHasWitness() const
{
    return std::any_of(vin.begin(), vin.end(), [](const auto& input) {
        return !input.scriptWitness.IsNull();
    });
}

Txid CTransaction::ComputeHash() const
{
    return Txid::FromUint256((HashWriter{} << TX_NO_WITNESS(*this)).GetHash());
}

Wtxid CTransaction::ComputeWitnessHash() const
{
    if (!HasWitness()) {
        return Wtxid::FromUint256(hash.ToUint256());
    }

    return Wtxid::FromUint256((HashWriter{} << TX_WITH_WITNESS(*this)).GetHash());
}

CTransaction::CTransaction(const CMutableTransaction& tx) : vin(tx.vin), vout(tx.vout), version{tx.version}, nLockTime{tx.nLockTime}, m_has_witness{ComputeHasWitness()}, hash{ComputeHash()}, m_witness_hash{ComputeWitnessHash()} {}
CTransaction::CTransaction(CMutableTransaction&& tx) : vin(std::move(tx.vin)), vout(std::move(tx.vout)), version{tx.version}, nLockTime{tx.nLockTime}, m_has_witness{ComputeHasWitness()}, hash{ComputeHash()}, m_witness_hash{ComputeWitnessHash()} {}

CAmount CTransaction::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (const auto& tx_out : vout) {
        if (!MoneyRange(tx_out.nValue) || !MoneyRange(nValueOut + tx_out.nValue))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
        nValueOut += tx_out.nValue;
    }
    assert(MoneyRange(nValueOut));
    return nValueOut;
}

unsigned int CTransaction::ComputeTotalSize() const
{
    return ::GetSerializeSize(TX_WITH_WITNESS(*this));
}

std::string CTransaction::ToString() const
{
    std::string str;
    str += strprintf("CTransaction(hash=%s, ver=%u, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
        GetHash().ToString().substr(0,10),
        version,
        vin.size(),
        vout.size(),
        nLockTime);
    for (const auto& tx_in : vin)
        str += "    " + tx_in.ToString() + "\n";
    for (const auto& tx_in : vin)
        str += "    " + tx_in.scriptWitness.ToString() + "\n";
    for (const auto& tx_out : vout)
        str += "    " + tx_out.ToString() + "\n";
    return str;
}
