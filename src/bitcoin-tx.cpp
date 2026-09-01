// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <connectcoin-build-config.h> // IWYU pragma: keep

#include <chainparamsbase.h>
#include <clientversion.h>
#include <coins.h>
#include <common/args.h>
#include <common/license_info.h>
#include <common/system.h>
#include <compat/compat.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/p2c.h>
#include <core_io.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <univalue.h>
#include <util/exception.h>
#include <util/fs.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>

#include <cstdio>
#include <functional>
#include <memory>

using util::SplitString;
using util::TrimString;
using util::TrimStringView;

static bool fCreateBlank;
static std::map<std::string,UniValue> registers;
static const int CONTINUE_EXECUTION=-1;

const TranslateFn G_TRANSLATION_FUN{nullptr};

static void SetupBitcoinTxArgs(ArgsManager &argsman)
{
    SetupHelpOptions(argsman);

    argsman.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-create", "Create new, empty TX.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-json", "Select JSON output", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-txid", "Output only the hex-encoded transaction id of the resultant transaction.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    SetupChainParamsBaseOptions(argsman);

    argsman.AddArg("delin=N", "Delete input N from TX", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("delout=N", "Delete output N from TX", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("in=TXID:VOUT(:SEQUENCE_NUMBER)", "Add input to TX", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("locktime=N", "Set TX lock time to N", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("nversion=N", "Set TX version to N", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outaddr=VALUE:ADDRESS", "Add address-based output to TX", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outdata=[VALUE:]DATA", "Unsupported: ConnectCoin has no data/Script output type", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outmultisig=...", "Unsupported: ConnectCoin has no multisig output type", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outpubkey=VALUE:PUBKEY", "Add a type-1 P2PK output from a 32-byte x-only or full secp256k1 public key", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outp2c=VALUE:DOMAIN:TARGET:ROOTS_VERSION", "Add a type-2 PAY_TO_CONNECT output", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("outscript=...", "Unsupported: ConnectCoin has no raw Script output type", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("p2cproof=INPUT_INDEX:PROOF", "Set the complete P2C proof witness for one input", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("replaceable(=N)", "Sets Replace-By-Fee (RBF) opt-in sequence number for input N. "
        "If N is not provided, the command attempts to opt-in all available inputs for RBF. "
        "If the transaction has no inputs, this option is ignored.", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);
    argsman.AddArg("sign=SIGHASH-FLAGS", "Add zero or more signatures to transaction. "
        "This command requires JSON registers:"
        "prevtxs=JSON object, "
        "privatekeys=JSON object. "
        "See signrawtransactionwithkey docs for format of sighash flags, JSON objects.", ArgsManager::ALLOW_ANY, OptionsCategory::COMMANDS);

    argsman.AddArg("load=NAME:FILENAME", "Load JSON file FILENAME into register NAME", ArgsManager::ALLOW_ANY, OptionsCategory::REGISTER_COMMANDS);
    argsman.AddArg("set=NAME:JSON-STRING", "Set register NAME to given JSON-STRING", ArgsManager::ALLOW_ANY, OptionsCategory::REGISTER_COMMANDS);
}

//
// This function returns either one of EXIT_ codes when it's expected to stop the process or
// CONTINUE_EXECUTION when it's expected to continue further.
//
static int AppInitRawTx(int argc, char* argv[])
{
    SetupBitcoinTxArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        tfm::format(std::cerr, "Error parsing command line arguments: %s\n", error);
        return EXIT_FAILURE;
    }

    // Check for chain settings (Params() calls are only valid after this clause)
    try {
        SelectParams(gArgs.GetChainType());
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    fCreateBlank = gArgs.GetBoolArg("-create", false);

    if (argc < 2 || HelpRequested(gArgs) || gArgs.GetBoolArg("-version", false)) {
        // First part of help message is specific to this utility
        std::string strUsage = CLIENT_NAME " connectcoin-tx utility version " + FormatFullVersion() + "\n";

        if (gArgs.GetBoolArg("-version", false)) {
            strUsage += FormatParagraph(LicenseInfo());
        } else {
            strUsage += "\n"
                "The connectcoin-tx tool is used for creating and modifying ConnectCoin transactions.\n\n"
                "connectcoin-tx can be used with \"<hex-tx> [commands]\" to update a hex-encoded ConnectCoin transaction, or with \"-create [commands]\" to create a hex-encoded ConnectCoin transaction.\n"
                "\n"
                "Usage: connectcoin-tx [options] <hex-tx> [commands]\n"
                "or:    connectcoin-tx [options] -create [commands]\n"
                "\n";
            strUsage += gArgs.GetHelpMessage();
        }

        tfm::format(std::cout, "%s", strUsage);

        if (argc < 2) {
            tfm::format(std::cerr, "Error: too few parameters\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    return CONTINUE_EXECUTION;
}

static void RegisterSetJson(const std::string& key, const std::string& rawJson)
{
    UniValue val;
    if (!val.read(rawJson)) {
        std::string strErr = "Cannot parse JSON for key " + key;
        throw std::runtime_error(strErr);
    }

    registers[key] = val;
}

static void RegisterSet(const std::string& strInput)
{
    // separate NAME:VALUE in string
    size_t pos = strInput.find(':');
    if ((pos == std::string::npos) ||
        (pos == 0) ||
        (pos == (strInput.size() - 1)))
        throw std::runtime_error("Register input requires NAME:VALUE");

    std::string key = strInput.substr(0, pos);
    std::string valStr = strInput.substr(pos + 1, std::string::npos);

    RegisterSetJson(key, valStr);
}

static void RegisterLoad(const std::string& strInput)
{
    // separate NAME:FILENAME in string
    size_t pos = strInput.find(':');
    if ((pos == std::string::npos) ||
        (pos == 0) ||
        (pos == (strInput.size() - 1)))
        throw std::runtime_error("Register load requires NAME:FILENAME");

    std::string key = strInput.substr(0, pos);
    std::string filename = strInput.substr(pos + 1, std::string::npos);

    FILE *f = fsbridge::fopen(filename.c_str(), "r");
    if (!f) {
        std::string strErr = "Cannot open file " + filename;
        throw std::runtime_error(strErr);
    }

    // load file chunks into one big buffer
    std::string valStr;
    while ((!feof(f)) && (!ferror(f))) {
        char buf[4096];
        int bread = fread(buf, 1, sizeof(buf), f);
        if (bread <= 0)
            break;

        valStr.insert(valStr.size(), buf, bread);
    }

    int error = ferror(f);
    fclose(f);

    if (error) {
        std::string strErr = "Error reading file " + filename;
        throw std::runtime_error(strErr);
    }

    // evaluate as JSON buffer register
    RegisterSetJson(key, valStr);
}

static CAmount ExtractAndValidateValue(const std::string& strValue)
{
    if (std::optional<CAmount> parsed = ParseMoney(strValue)) {
        return parsed.value();
    } else {
        throw std::runtime_error("invalid TX output value");
    }
}

static void MutateTxVersion(CMutableTransaction& tx, const std::string& cmdVal)
{
    const auto ver{ToIntegral<uint32_t>(cmdVal)};
    if (!ver || *ver < 1 || *ver > TX_MAX_STANDARD_VERSION) {
        throw std::runtime_error("Invalid TX version requested: '" + cmdVal + "'");
    }
    tx.version = *ver;
}

static void MutateTxLocktime(CMutableTransaction& tx, const std::string& cmdVal)
{
    const auto locktime{ToIntegral<uint32_t>(cmdVal)};
    if (!locktime) {
        throw std::runtime_error("Invalid TX locktime requested: '" + cmdVal + "'");
    }
    tx.nLockTime = *locktime;
}

static void MutateTxRBFOptIn(CMutableTransaction& tx, const std::string& strInIdx)
{
    const auto idx{ToIntegral<uint32_t>(strInIdx)};
    if (strInIdx != "" && (!idx || *idx >= tx.vin.size())) {
        throw std::runtime_error("Invalid TX input index '" + strInIdx + "'");
    }

    // set the nSequence to MAX_INT - 2 (= RBF opt in flag)
    uint32_t cnt{0};
    for (CTxIn& txin : tx.vin) {
        if (strInIdx == "" || cnt == *idx) {
            if (txin.nSequence > MAX_BIP125_RBF_SEQUENCE) {
                txin.nSequence = MAX_BIP125_RBF_SEQUENCE;
            }
        }
        ++cnt;
    }
}

template <typename T>
static T TrimAndParse(const std::string& int_str, const std::string& err)
{
    const auto parsed{ToIntegral<T>(TrimStringView(int_str))};
    if (!parsed.has_value()) {
        throw std::runtime_error(err + " '" + int_str + "'");
    }
    return parsed.value();
}

static void MutateTxAddInput(CMutableTransaction& tx, const std::string& strInput)
{
    std::vector<std::string> vStrInputParts = SplitString(strInput, ':');

    // separate TXID:VOUT in string
    if (vStrInputParts.size()<2)
        throw std::runtime_error("TX input missing separator");

    // extract and validate TXID
    auto txid{Txid::FromHex(vStrInputParts[0])};
    if (!txid) {
        throw std::runtime_error("invalid TX input txid");
    }

    static const unsigned int minTxOutSz = 9;
    static const unsigned int maxVout = MAX_BLOCK_WEIGHT / (WITNESS_SCALE_FACTOR * minTxOutSz);

    // extract and validate vout
    const std::string& strVout = vStrInputParts[1];
    const auto vout{ToIntegral<uint32_t>(strVout)};
    if (!vout || *vout > maxVout) {
        throw std::runtime_error("invalid TX input vout '" + strVout + "'");
    }

    // extract the optional sequence number
    uint32_t nSequenceIn = CTxIn::SEQUENCE_FINAL;
    if (vStrInputParts.size() > 2) {
        nSequenceIn = TrimAndParse<uint32_t>(vStrInputParts.at(2), "invalid TX sequence id");
    }

    // append to transaction input list
    CTxIn txin{*txid, *vout, CScript{}, nSequenceIn};
    tx.vin.push_back(txin);
}

static void MutateTxAddOutAddr(CMutableTransaction& tx, const std::string& strInput)
{
    // Separate into VALUE:ADDRESS
    std::vector<std::string> vStrInputParts = SplitString(strInput, ':');

    if (vStrInputParts.size() != 2)
        throw std::runtime_error("TX output missing or too many separators");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // extract and validate ADDRESS
    const std::string& strAddr = vStrInputParts[1];
    CTxDestination destination = DecodeDestination(strAddr);
    if (!IsValidDestination(destination)) {
        throw std::runtime_error("invalid TX output address");
    }
    if (!std::holds_alternative<WitnessV1Taproot>(destination)) {
        throw std::runtime_error("ConnectCoin outputs require a type-1 bech32m address");
    }
    CScript scriptPubKey = GetScriptForDestination(destination);

    // construct TxOut, append to transaction output list
    CTxOut txout(value, scriptPubKey);
    tx.vout.push_back(txout);
}

static void MutateTxAddOutPubKey(CMutableTransaction& tx, const std::string& strInput)
{
    // Separate into VALUE:PUBKEY
    std::vector<std::string> vStrInputParts = SplitString(strInput, ':');

    if (vStrInputParts.size() != 2)
        throw std::runtime_error("TX output missing or too many separators");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // Extract and validate PUBKEY
    const std::vector<unsigned char> encoded{ParseHex(vStrInputParts[1])};
    XOnlyPubKey pubkey;
    if (encoded.size() == XOnlyPubKey::size()) {
        pubkey = XOnlyPubKey{encoded};
    } else {
        const CPubKey full_pubkey{encoded};
        if (!full_pubkey.IsFullyValid()) throw std::runtime_error("invalid TX output pubkey");
        pubkey = XOnlyPubKey{full_pubkey};
    }
    if (!pubkey.IsFullyValid()) throw std::runtime_error("invalid TX output x-only pubkey");

    // construct TxOut, append to transaction output list
    CTxOut txout(value, pubkey);
    tx.vout.push_back(txout);
}

static void MutateTxAddOutP2C(CMutableTransaction& tx, const std::string& strInput)
{
    const std::vector<std::string> parts{SplitString(strInput, ':')};
    if (parts.size() != 4) {
        throw std::runtime_error("P2C output must be VALUE:DOMAIN:TARGET:ROOTS_VERSION");
    }
    const CAmount value{ExtractAndValidateValue(parts[0])};
    if (!IsCanonicalP2CDomain(parts[1])) {
        throw std::runtime_error("P2C domain must be canonical lower-case ASCII without a trailing dot");
    }
    const auto target{uint256::FromHex(parts[2])};
    if (!target) throw std::runtime_error("P2C target must be exactly 32 bytes of hex");
    const auto roots_version{ToIntegral<uint32_t>(parts[3])};
    if (!roots_version || !IsSupportedP2CRootCertificatesVersion(*roots_version)) {
        throw std::runtime_error("unsupported P2C root certificate version");
    }
    tx.vout.emplace_back(value, PayToDomainOutput{
        .domain = parts[1],
        .connection_work_target = *target,
        .root_certificates_version = *roots_version,
    });
}

static void MutateTxSetP2CProof(CMutableTransaction& tx, const std::string& strInput)
{
    const size_t separator{strInput.find(':')};
    if (separator == std::string::npos) throw std::runtime_error("P2C proof must be INPUT_INDEX:PROOF");
    const auto input_index{ToIntegral<uint32_t>(strInput.substr(0, separator))};
    if (!input_index || *input_index >= tx.vin.size()) throw std::runtime_error("invalid P2C proof input index");
    const std::string_view proof_hex{strInput.c_str() + separator + 1, strInput.size() - separator - 1};
    if (proof_hex.empty() || proof_hex.size() > MAX_P2C_PROOF_SIZE * 2 || !IsHex(proof_hex)) {
        throw std::runtime_error("P2C proof must be non-empty hexadecimal data within the consensus size limit");
    }
    CTxIn& input{tx.vin[*input_index]};
    if (!input.scriptSig.empty()) throw std::runtime_error("P2C input scriptSig must be empty");
    input.scriptWitness.stack = {ParseHex(proof_hex)};
}

static void MutateTxAddOutMultiSig(CMutableTransaction& tx, const std::string& strInput)
{
    (void)tx;
    (void)strInput;
    throw std::runtime_error("ConnectCoin typed outputs do not support multisig");
}

static void MutateTxAddOutData(CMutableTransaction& tx, const std::string& strInput)
{
    (void)tx;
    (void)strInput;
    throw std::runtime_error("ConnectCoin typed outputs do not support data/OP_RETURN");
}

static void MutateTxAddOutScript(CMutableTransaction& tx, const std::string& strInput)
{
    (void)tx;
    (void)strInput;
    throw std::runtime_error("ConnectCoin has no raw Script output type; use outaddr or outpubkey");
}

static void MutateTxDelInput(CMutableTransaction& tx, const std::string& strInIdx)
{
    const auto idx{ToIntegral<uint32_t>(strInIdx)};
    if (!idx || idx >= tx.vin.size()) {
        throw std::runtime_error("Invalid TX input index '" + strInIdx + "'");
    }
    tx.vin.erase(tx.vin.begin() + *idx);
}

static void MutateTxDelOutput(CMutableTransaction& tx, const std::string& strOutIdx)
{
    const auto idx{ToIntegral<uint32_t>(strOutIdx)};
    if (!idx || idx >= tx.vout.size()) {
        throw std::runtime_error("Invalid TX output index '" + strOutIdx + "'");
    }
    tx.vout.erase(tx.vout.begin() + *idx);
}

static const unsigned int N_SIGHASH_OPTS = 1;
static const struct {
    const char *flagStr;
    int flags;
} sighashOptions[N_SIGHASH_OPTS] = {
    {"DEFAULT", SIGHASH_DEFAULT},
};

static bool findSighashFlags(int& flags, const std::string& flagStr)
{
    flags = 0;

    for (unsigned int i = 0; i < N_SIGHASH_OPTS; i++) {
        if (flagStr == sighashOptions[i].flagStr) {
            flags = sighashOptions[i].flags;
            return true;
        }
    }

    return false;
}

static CAmount AmountFromValue(const UniValue& value)
{
    if (!value.isNum() && !value.isStr())
        throw std::runtime_error("Amount is not a number or string");
    int64_t amount;
    if (!ParseFixedPoint(value.getValStr(), 10, &amount))
        throw std::runtime_error("Invalid amount");
    if (!MoneyRange(amount))
        throw std::runtime_error("Amount out of range");
    return amount;
}

static std::vector<unsigned char> ParseHexUV(const UniValue& v, const std::string& strName)
{
    std::string strHex;
    if (v.isStr())
        strHex = v.getValStr();
    if (!IsHex(strHex))
        throw std::runtime_error(strName + " must be hexadecimal string (not '" + strHex + "')");
    return ParseHex(strHex);
}

static void MutateTxSign(CMutableTransaction& tx, const std::string& flagStr)
{
    int nHashType = SIGHASH_DEFAULT;

    if (flagStr.size() > 0)
        if (!findSighashFlags(nHashType, flagStr))
            throw std::runtime_error("unknown sighash flag/sign option");

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the raw tx:
    CMutableTransaction mergedTx{tx};
    CCoinsViewCache view{&CoinsViewEmpty::Get()};

    if (!registers.contains("privatekeys"))
        throw std::runtime_error("privatekeys register variable must be set.");
    FillableSigningProvider tempKeystore;
    UniValue keysObj = registers["privatekeys"];

    for (unsigned int kidx = 0; kidx < keysObj.size(); kidx++) {
        if (!keysObj[kidx].isStr())
            throw std::runtime_error("privatekey not a std::string");
        CKey key = DecodeSecret(keysObj[kidx].getValStr());
        if (!key.IsValid()) {
            throw std::runtime_error("privatekey not valid");
        }
        tempKeystore.AddKey(key);
    }

    // Add previous txouts given in the RPC call:
    if (!registers.contains("prevtxs"))
        throw std::runtime_error("prevtxs register variable must be set.");
    UniValue prevtxsObj = registers["prevtxs"];
    {
        for (unsigned int previdx = 0; previdx < prevtxsObj.size(); previdx++) {
            const UniValue& prevOut = prevtxsObj[previdx];
            if (!prevOut.isObject())
                throw std::runtime_error("expected prevtxs internal object");

            std::map<std::string, UniValue::VType> types = {
                {"txid", UniValue::VSTR},
                {"vout", UniValue::VNUM},
                {"scriptPubKey", UniValue::VSTR},
            };
            if (!prevOut.checkObject(types))
                throw std::runtime_error("prevtxs internal object typecheck fail");

            auto txid{Txid::FromHex(prevOut["txid"].get_str())};
            if (!txid) {
                throw std::runtime_error("txid must be hexadecimal string (not '" + prevOut["txid"].get_str() + "')");
            }

            const int nOut = prevOut["vout"].getInt<int>();
            if (nOut < 0)
                throw std::runtime_error("vout cannot be negative");

            COutPoint out(*txid, nOut);
            std::vector<unsigned char> pkData(ParseHexUV(prevOut["scriptPubKey"], "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                const Coin& coin = view.AccessCoin(out);
                if (!coin.IsSpent() && coin.out.scriptPubKey != scriptPubKey) {
                    std::string err("Previous output scriptPubKey mismatch:\n");
                    err = err + ScriptToAsmStr(coin.out.scriptPubKey) + "\nvs:\n"+
                        ScriptToAsmStr(scriptPubKey);
                    throw std::runtime_error(err);
                }
                Coin newcoin;
                newcoin.out.SetScriptPubKey(scriptPubKey);
                const bool p2pk{newcoin.out.GetType() == TxOutputType::P2PK && newcoin.out.GetP2PKPubKey().has_value()};
                if (!p2pk && !IsCanonicalP2COutput(newcoin.out)) {
                    throw std::runtime_error("prevtxs scriptPubKey must encode a canonical ConnectCoin typed output");
                }
                newcoin.out.nValue = MAX_MONEY;
                if (prevOut.exists("amount")) {
                    newcoin.out.nValue = AmountFromValue(prevOut["amount"]);
                }
                newcoin.nHeight = 1;
                view.AddCoin(out, std::move(newcoin), true);
            }

            // if redeemScript given and private keys given,
            // add redeemScript to the tempKeystore so it can be signed:
            if ((scriptPubKey.IsPayToScriptHash() || scriptPubKey.IsPayToWitnessScriptHash()) &&
                prevOut.exists("redeemScript")) {
                UniValue v = prevOut["redeemScript"];
                std::vector<unsigned char> rsData(ParseHexUV(v, "redeemScript"));
                CScript redeemScript(rsData.begin(), rsData.end());
                tempKeystore.AddCScript(redeemScript);
            }
        }
    }

    const FillableSigningProvider& keystore = tempKeystore;

    // Type-1 signatures use the BIP341 transaction digest, which commits to
    // every input's prevout amount and authorization key. Build the complete
    // spent-output set before signing any input; a partial prevtxs register
    // cannot produce a valid SIGHASH_DEFAULT signature.
    const CTransaction tx_const{mergedTx};
    std::vector<CTxOut> spent_outputs;
    spent_outputs.reserve(mergedTx.vin.size());
    for (const CTxIn& txin : mergedTx.vin) {
        const Coin& coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            throw std::runtime_error("prevtxs must contain every transaction input for type-1 signing");
        }
        if (coin.out.nValue == MAX_MONEY) {
            throw std::runtime_error(strprintf("Missing amount for CTxOut with scriptPubKey=%s", HexStr(coin.out.scriptPubKey)));
        }
        spent_outputs.push_back(coin.out);
    }
    PrecomputedTransactionData txdata;
    txdata.Init(tx_const, std::move(spent_outputs), /*force=*/true);

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++) {
        CTxIn& txin = mergedTx.vin[i];
        const Coin& coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            continue;
        }
        const CScript& prevPubKey = coin.out.scriptPubKey;
        const CAmount& amount = coin.out.nValue;

        if (IsCanonicalP2COutput(coin.out)) {
            if (!txin.scriptSig.empty() || txin.scriptWitness.stack.size() != 1 ||
                txin.scriptWitness.stack.front().empty() ||
                txin.scriptWitness.stack.front().size() > MAX_P2C_PROOF_SIZE) {
                throw std::runtime_error("P2C input requires one complete proof witness");
            }
            continue;
        }

        SignatureData sigdata = DataFromTransaction(mergedTx, i, coin.out);
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.vout.size()))
            ProduceSignature(keystore, MutableTransactionSignatureCreator(mergedTx, i, amount, &txdata, {.sighash_type = nHashType}), prevPubKey, sigdata);

        if (amount == MAX_MONEY && !sigdata.scriptWitness.IsNull()) {
            throw std::runtime_error(strprintf("Missing amount for CTxOut with scriptPubKey=%s", HexStr(prevPubKey)));
        }

        UpdateInput(txin, sigdata);
        if (!txin.scriptSig.empty() || txin.scriptWitness.stack.size() != 1 ||
            txin.scriptWitness.stack.front().size() != 64) {
            throw std::runtime_error("sign did not produce a complete type-1 SIGHASH_DEFAULT witness");
        }
        ScriptError error{SCRIPT_ERR_OK};
        if (!VerifyScript(txin.scriptSig, prevPubKey, &txin.scriptWitness,
                          STANDARD_SCRIPT_VERIFY_FLAGS,
                          TransactionSignatureChecker{&tx_const, i, amount, txdata, MissingDataBehavior::FAIL},
                          &error)) {
            throw std::runtime_error(strprintf("sign did not produce a valid type-1 witness: %s", ScriptErrorString(error)));
        }
    }

    tx = mergedTx;
}

static void MutateTx(CMutableTransaction& tx, const std::string& command,
                     const std::string& commandVal)
{
    std::unique_ptr<ECC_Context> ecc;

    if (command == "nversion")
        MutateTxVersion(tx, commandVal);
    else if (command == "locktime")
        MutateTxLocktime(tx, commandVal);
    else if (command == "replaceable") {
        MutateTxRBFOptIn(tx, commandVal);
    }

    else if (command == "delin")
        MutateTxDelInput(tx, commandVal);
    else if (command == "in")
        MutateTxAddInput(tx, commandVal);

    else if (command == "delout")
        MutateTxDelOutput(tx, commandVal);
    else if (command == "outaddr")
        MutateTxAddOutAddr(tx, commandVal);
    else if (command == "outpubkey") {
        ecc.reset(new ECC_Context());
        MutateTxAddOutPubKey(tx, commandVal);
    } else if (command == "outp2c") {
        MutateTxAddOutP2C(tx, commandVal);
    } else if (command == "outmultisig") {
        ecc.reset(new ECC_Context());
        MutateTxAddOutMultiSig(tx, commandVal);
    } else if (command == "outscript")
        MutateTxAddOutScript(tx, commandVal);
    else if (command == "outdata")
        MutateTxAddOutData(tx, commandVal);
    else if (command == "p2cproof")
        MutateTxSetP2CProof(tx, commandVal);

    else if (command == "sign") {
        ecc.reset(new ECC_Context());
        MutateTxSign(tx, commandVal);
    }

    else if (command == "load")
        RegisterLoad(commandVal);

    else if (command == "set")
        RegisterSet(commandVal);

    else
        throw std::runtime_error("unknown command");
}

static void OutputTxJSON(const CTransaction& tx)
{
    UniValue entry(UniValue::VOBJ);
    TxToUniv(tx, /*block_hash=*/uint256(), entry);

    std::string jsonOutput = entry.write(4);
    tfm::format(std::cout, "%s\n", jsonOutput);
}

static void OutputTxHash(const CTransaction& tx)
{
    std::string strHexHash = tx.GetHash().GetHex(); // the hex-encoded transaction hash (aka the transaction id)

    tfm::format(std::cout, "%s\n", strHexHash);
}

static void OutputTxHex(const CTransaction& tx)
{
    std::string strHex = EncodeHexTx(tx);

    tfm::format(std::cout, "%s\n", strHex);
}

static void OutputTx(const CTransaction& tx)
{
    if (gArgs.GetBoolArg("-json", false))
        OutputTxJSON(tx);
    else if (gArgs.GetBoolArg("-txid", false))
        OutputTxHash(tx);
    else
        OutputTxHex(tx);
}

static std::string readStdin()
{
    char buf[4096];
    std::string ret;

    while (!feof(stdin)) {
        size_t bread = fread(buf, 1, sizeof(buf), stdin);
        ret.append(buf, bread);
        if (bread < sizeof(buf))
            break;
    }

    if (ferror(stdin))
        throw std::runtime_error("error reading stdin");

    return TrimString(ret);
}

static int CommandLineRawTx(int argc, char* argv[])
{
    std::string strPrint;
    int nRet = 0;
    try {
        // Skip switches; Permit common stdin convention "-"
        while (argc > 1 && IsSwitchChar(argv[1][0]) &&
               (argv[1][1] != 0)) {
            argc--;
            argv++;
        }

        CMutableTransaction tx;
        int startArg;

        if (!fCreateBlank) {
            // require at least one param
            if (argc < 2)
                throw std::runtime_error("too few parameters");

            // param: hex-encoded ConnectCoin transaction
            std::string strHexTx(argv[1]);
            if (strHexTx == "-")                 // "-" implies standard input
                strHexTx = readStdin();

            if (!DecodeHexTx(tx, strHexTx, true))
                throw std::runtime_error("invalid transaction encoding");

            startArg = 2;
        } else
            startArg = 1;

        for (int i = startArg; i < argc; i++) {
            std::string arg = argv[i];
            std::string key, value;
            size_t eqpos = arg.find('=');
            if (eqpos == std::string::npos)
                key = arg;
            else {
                key = arg.substr(0, eqpos);
                value = arg.substr(eqpos + 1);
            }

            MutateTx(tx, key, value);
        }

        OutputTx(CTransaction(tx));
    }
    catch (const std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    }
    catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRawTx()");
        throw;
    }

    if (strPrint != "") {
        tfm::format(nRet == 0 ? std::cout : std::cerr, "%s\n", strPrint);
    }
    return nRet;
}

MAIN_FUNCTION
{
    SetupEnvironment();

    try {
        int ret = AppInitRawTx(argc, argv);
        if (ret != CONTINUE_EXECUTION)
            return ret;
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInitRawTx()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(nullptr, "AppInitRawTx()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRawTx(argc, argv);
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRawTx()");
    } catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRawTx()");
    }
    return ret;
}
