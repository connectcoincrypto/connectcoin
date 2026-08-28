// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CONNECTCOIN_EXTERNAL_SIGNER_H
#define CONNECTCOIN_EXTERNAL_SIGNER_H

#include <common/system.h>
#include <univalue.h>

#include <string>
#include <vector>

class PartiallySignedTransaction;

//! Enables interaction with a signing device or service that explicitly
//! implements ConnectCoin's typed-output protocol. See doc/external-signer.md
class ExternalSigner
{
private:
    //! The command which handles interaction with the external signer.
    std::vector<std::string> m_command;

    //! ConnectCoin mainnet, testnet, etc.
    std::string m_chain;

    std::vector<std::string> NetworkArg() const;

public:
    //! @param[in] command      the command which handles interaction with the external signer
    //! @param[in] fingerprint  master key fingerprint of the signer
    //! @param[in] chain        "main", "test", "signet", "regtest" or "testnet4"
    //! @param[in] name         device name
    ExternalSigner(std::vector<std::string> command, std::string chain, std::string fingerprint, std::string name, bool supports_typed_outputs);

    //! Master key fingerprint of the signer
    std::string m_fingerprint;

    //! Name of signer
    std::string m_name;

    //! Whether the signer explicitly advertises ConnectCoin's typed-output
    //! PSBT and signature-digest protocol.
    bool m_supports_typed_outputs;

    //! Obtain a list of signers. Calls `<command> enumerate`.
    //! @param[in]              command the command which handles interaction with the external signer
    //! @param[in,out] signers  vector to which new signers (with a unique master key fingerprint) are added
    //! @param chain            "main", "test", "signet", "regtest" or "testnet4"
    //! @returns success
    static bool Enumerate(const std::string& command, std::vector<ExternalSigner>& signers, const std::string& chain);

    //! Display address on the device. Calls `<command> --fingerprint <fingerprint> --chain <chain>
    //! displayaddress --desc <descriptor>`.
    //! @param[in] descriptor Descriptor specifying which address to display.
    //!            Must include a public key or xpub, as well as key origin.
    UniValue DisplayAddress(const std::string& descriptor) const;

    //! Get receive and change Descriptor(s) from device for a given account.
    //! Calls `<command> --fingerprint <fingerprint> --chain <chain> getdescriptors
    //! --account <account>`.
    //! @param[in] account  which BIP32 account to use (e.g. `m/44'/0'/account'`)
    //! @returns see doc/external-signer.md
    UniValue GetDescriptors(int account);

    //! Sign PartiallySignedTransaction on the device.
    //! Calls `<command> --stdin --fingerprint <fingerprint> --chain <chain>` and passes the
    //! `signtx` command and PSBT via stdin.
    //! @param[in,out] psbt  PartiallySignedTransaction to be signed
    bool SignTransaction(PartiallySignedTransaction& psbt, std::string& error);
};

#endif // CONNECTCOIN_EXTERNAL_SIGNER_H
