# Offline Signing Tutorial

This tutorial uses an online watch-only wallet to prepare a transaction and an
offline wallet to sign it. The private keys remain on the offline host.

The workflow uses ConnectCoin's [PSBT dialect](psbt.md) and type-1 P2PK
addresses. Both hosts must run the same current ConnectCoin Core build;
Bitcoin PSBT tools and example transactions are not compatible. P2C redemption
proofs use a separate workflow described in [p2c-wallet.md](p2c-wallet.md).

> [!NOTE]
> This tutorial explicitly selects ConnectCoin signet on both hosts. Keep the
> same network selection in every command. Omitting `-signet` selects the
> default Testnet4 beta, not mainnet. Mainnet startup is unavailable.

## Overview
In this tutorial we have two hosts, both running the same current ConnectCoin Core build.

* `offline` host which is disconnected from all networks (internet, Tor, wifi, bluetooth etc.) and does not have, or need, a copy of the blockchain.
* `online` host which is a regular online node with a synced blockchain.

We are going to first create an `offline_wallet` on the offline host. We will then create a `watch_only_wallet` on the online host using a wallet file exported from the `offline_wallet`. Next we will receive some coins into the wallet. In order to spend these coins we'll create an unsigned PSBT using the `watch_only_wallet`, sign the PSBT using the private keys in the `offline_wallet`, and finally broadcast the signed PSBT using the online host.

### Requirements

- A Bash-compatible shell and [jq](https://jqlang.github.io/jq/) on both hosts.
- A running ConnectCoin node on each host, with `-signet` selected. Keep the
  offline host disconnected from all networks; its node can additionally use
  `-networkactive=0 -listen=0 -dnsseed=0`. It does not need the blockchain.
- A synced online signet node and a source of ConnectCoin signet test coins.

The `[offline]$` and `[online]$` labels below identify the host and are not part
of the commands. Replace `/path/to/` with local file paths. Use a separate set
of transaction files for each payment, and stop if any command fails.

Enable pipeline error reporting in each shell:

```sh
set -o pipefail
```

### Create and Prepare the `offline_wallet`

1. On the offline machine create a wallet named `offline_wallet` secured by a wallet `passphrase`. This wallet will contain private keys and must remain unconnected to any networks at all times.

```sh
[offline]$ ./build/bin/connectcoin-cli -signet -stdin -named createwallet \
                wallet_name="offline_wallet"
```

At the waiting input, enter `passphrase=<your chosen passphrase>` on one line,
then finish input with Ctrl-D. This supplies the passphrase through standard
input instead of putting it in the command line or shell history. This input
mode does not hide what you type, so use a private console.

> [!NOTE]
> Encryption protects the private keys while the wallet is locked; it does not
> encrypt transaction history or other public metadata. Keep the wallet file,
> its backups, and the passphrase offline. See
> [Managing the Wallet](managing-wallets.md#12-encrypting-the-wallet).

2. Export the wallet in a watch-only format to a .dat file named `watch_only_wallet.dat`.
```sh
[offline]$ ./build/bin/connectcoin-cli -signet -rpcwallet="offline_wallet" -named exportwatchonlywallet \
             destination=/path/to/watch_only_wallet.dat
```

> [!NOTE]
> Transfer only the exported `watch_only_wallet.dat` to the online machine
> (for example, using removable media). Do not transfer the original wallet or
> its private-key backups. The exported file contains public wallet data.

### Create the online `watch_only_wallet`

On the online machine import the watch-only wallet. This wallet will have the private keys disabled and is named `watch_only_wallet`. This is achieved by using the `restorewallet` rpc call.
The `watch_only_wallet` wallet will be used to track and validate incoming transactions, create unsigned PSBTs when spending coins, and broadcast signed and finalized PSBTs.

```sh
[online]$ ./build/bin/connectcoin-cli -signet -named restorewallet \
              wallet_name="watch_only_wallet" \
              backup_file=/path/to/watch_only_wallet.dat
```

### Fund the `offline_wallet`

At this point, it's important to understand that both the `offline_wallet` and online `watch_only_wallet` share the same public keys. As a result, they generate the same addresses. Transactions can be created using either wallet, but valid signatures can only be added by the `offline_wallet` as only it has the private keys.

1. Generate an address to receive coins. You can use _either_ the `offline_wallet` or the online `watch_only_wallet` to generate this address, as they will produce the same addresses. For the sake of this guide, we'll use the online `watch_only_wallet` to generate the address.

```sh
[online]$ ./build/bin/connectcoin-cli -signet -rpcwallet="watch_only_wallet" getnewaddress "" "bech32m"
```

Use the returned `tcc1p...` address; it is generated from this wallet's keys.

2. Fund the address from an existing ConnectCoin signet peer or faucet. No public ConnectCoin signet faucet is currently documented; Bitcoin signet faucets cannot fund this network.

3. Confirm that coins were received using the online `watch_only_wallet`. Note that the transaction may take a few moments before being received on your local node, depending on its connectivity. Just re-run the command periodically until the transaction is received.

```sh
[online]$ ./build/bin/connectcoin-cli -signet -rpcwallet="watch_only_wallet" listunspent
```

Check the actual received amounts and confirmations. For the example payment
below, wait for a confirmed balance greater than `0.009` CC plus the fee.

### Create and Export an Unsigned PSBT

1. Obtain a type-1 Bech32m receiving address from a ConnectCoin signet wallet
you control. Enter that actual destination below; do not use an address copied
from documentation.

2. Create a funded, unsigned PSBT with the online `watch_only_wallet`. The
example sends `0.009` CC and lets the wallet select a fee using its configured
policy and current network conditions. `psbt=true` requests a PSBT without
broadcasting. Review the actual fee on the offline host before signing.

```sh
[online]$ read -r -p "ConnectCoin signet recipient address: " recipient
[online]$ outputs=$(jq -n --arg address "$recipient" '{($address): 0.009}')
[online]$ ./build/bin/connectcoin-cli -signet -rpcwallet="watch_only_wallet" -named send \
              outputs="$outputs" psbt=true \
              | jq -er '.psbt' > /path/to/funded_psbt.txt
```

Transfer `funded_psbt.txt` to the offline host. This file contains the PSBT
returned for your transaction, including the input UTXO data needed for offline
signing.

### Decode and Analyze the Unsigned PSBT

Decode and analyze the unsigned PSBT on the `offline_wallet` using the `funded_psbt.txt` file:

```sh
[offline]$ ./build/bin/connectcoin-cli -signet decodepsbt "$(cat /path/to/funded_psbt.txt)"
[offline]$ ./build/bin/connectcoin-cli -signet analyzepsbt "$(cat /path/to/funded_psbt.txt)"
```

Inspect the actual inputs, destination addresses, amounts, change, and fee on
the offline host before signing. Confirm that change belongs to the offline
wallet, for example with `getaddressinfo`. Analysis describes the supplied
PSBT; it does not replace your approval of the payment. If required input data
is missing, return to the online host to prepare a complete unsigned PSBT.

### Process and Sign the PSBT

1. Unlock the `offline_wallet` with the Passphrase:

Use the walletpassphrase command to unlock the `offline_wallet` with the passphrase. You should specify the passphrase and a timeout (in seconds) for how long you want the wallet to remain unlocked.

```sh
[offline]$ ./build/bin/connectcoin-cli -signet -rpcwallet="offline_wallet" -stdinwalletpassphrase walletpassphrase 60
```

Enter the passphrase at the prompt. This input mode hides it from the terminal
and does not place it in the command line.

2. Process and sign the PSBT with the `offline_wallet`, then lock the wallet:

```sh
[offline]$ ./build/bin/connectcoin-cli -signet -rpcwallet="offline_wallet" walletprocesspsbt \
                "$(cat /path/to/funded_psbt.txt)" > /path/to/signed_psbt.json
[offline]$ ./build/bin/connectcoin-cli -signet -rpcwallet="offline_wallet" walletlock
```

The default signing mode uses ConnectCoin's required `SIGHASH_DEFAULT` and a
single 64-byte Schnorr witness per type-1 input. Extract the finalized raw
transaction only when the response reports `complete: true`:

```sh
[offline]$ jq -er 'if .complete then .hex else error("Transaction is not complete") end' \
                /path/to/signed_psbt.json > /path/to/signed_tx.hex
```

If signing fails, lock the wallet and resolve the error before proceeding.
Transfer `signed_tx.hex` to the online host only after successful extraction.

### Broadcast the Signed Transaction

The extracted file contains a raw transaction, not a PSBT. Decode it on the
online host to check the transaction being submitted, then broadcast it:

```sh
[online]$ ./build/bin/connectcoin-cli -signet decoderawtransaction "$(cat /path/to/signed_tx.hex)"
[online]$ ./build/bin/connectcoin-cli -signet sendrawtransaction "$(cat /path/to/signed_tx.hex)"
```

### Confirm Wallet Balance

Confirm the updated balance of the offline wallet using the `watch_only_wallet`.

```sh
[online]$ ./build/bin/connectcoin-cli -signet -rpcwallet="watch_only_wallet" getbalances
```

You can also show transactions related to the wallet using `listtransactions`

```sh
[online]$ ./build/bin/connectcoin-cli -signet -rpcwallet="watch_only_wallet" listtransactions
```
