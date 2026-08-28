# Support for signing transactions outside of ConnectCoin Core

ConnectCoin Core can be launched with `-signer=<cmd>` where `<cmd>` is an external tool that explicitly implements ConnectCoin's typed-output signing protocol.

Interaction with an external signer uses ConnectCoin's [non-interoperable PSBT dialect](psbt.md). A generic Bitcoin signer or Bitcoin Core HWI installation is **not compatible**: it may understand `tr()` descriptors while computing the wrong signature digest for ConnectCoin's type-1 outputs.

## Example usage

No production hardware-wallet integration is currently endorsed. Only use a signer whose `enumerate` response advertises `"protocol": "connectcoin-typed-v1"` and whose implementation has been tested end to end with ConnectCoin transactions. Merely wrapping an existing Bitcoin HWI command is insufficient.

Start ConnectCoin Core:

```sh
connectcoind -signer=/path/to/connectcoin-compatible-signer
```

`connectcoin node` can also be substituted for `connectcoind`.

### Create wallet and import keys

Get a list of signing devices / services:

```sh
connectcoin-cli enumeratesigners
```

```
{
  "signers": [
    {
      "fingerprint": "c8df832a",
      "name": "typed-v1-signer",
      "connectcoin_compatible": true
    }
  ]
}
```

The master key fingerprint identifies a signer. Wallet creation rejects entries where `connectcoin_compatible` is false.

Create a wallet, this automatically imports the public keys:

```sh
connectcoin rpc createwallet wallet_name="hww2" disable_private_keys=true descriptors=true external_signer=true
```

Creation of the external wallet can be confirmed with `getwalletinfo`, which will report `"external_signer": true`. These commands can also be executed using `connectcoin-qt` Debug Console instead of using `connectcoin rpc` or `connectcoin-cli`.

### Verify an address

Display an address on the device:

```sh
connectcoin-cli -rpcwallet=<walletname> getnewaddress
connectcoin-cli -rpcwallet=<walletname> walletdisplayaddress <address>
```

Replace `<address>` with the result of `getnewaddress`.

### Spending

Under the hood this uses a [PSBT (Partially Signed Bitcoin Transaction)](psbt.md).

```sh
connectcoin rpc -rpcwallet=<walletname> send outputs='{"<address>": <amount>}'
```

This constructs a ConnectCoin PSBT and prompts the external signer. ConnectCoin Core independently verifies and finalizes the returned type-1 Schnorr signature before broadcasting.

```
{"complete": true, "txid": "<txid>"}
```

## Signer API

To be compatible with ConnectCoin Core, a signer command must conform to the specification below, including the mandatory protocol capability. This is a ConnectCoin-specific interface and is not a Bitcoin BIP standard.

Prerequisite knowledge:
* [Output Descriptors](descriptors.md)
* Partially Signed Bitcoin Transaction ([PSBT](psbt.md))

### Flag `--chain <name>` (required except for `enumerate`)

With `<name>` one of `main`, `test`, `signet`, `regtest`, `testnet4`.

ConnectCoin Core passes this flag to commands that operate on a chain-specific signer, for example `getdescriptors`, `displayaddress` and `signtx`.

### Flag `--stdin` (required)

Indicate that (sub)command should be received over stdin and results returned in response to that. `--stdin` is a global flag, it is not specific to any subcommand.

ConnectCoin Core currently uses this flag for `signtx`.

All subcommands SHOULD support both
- being called as commandline arguments; or
- being written to _external-signer_ process directly through stdin (with `--stdin`).

Usage:
```sh
<cmd> --stdin …
```

```
[…command and arguments written to stdin…]
```

Note: remember that shell-expansion is not available on _stdin_. Consequently, commands such as `signtx`, may write their arguments in either quoted or unquoted form.

### Flag `--fingerprint <fingerprint>` (required except for `enumerate`)

With `<fingerprint>` being the hexadecimal 8-symbol identifier for a wallet.

ConnectCoin Core passes this flag to commands that operate on a specific external-signer wallet, for example `getdescriptors`, `displayaddress` and `signtx`.

### `enumerate` (required)

Usage:

```sh
<cmd> enumerate
```

```
[
    {
        "fingerprint": "00000000",
        "model": "typed-v1-signer",
        "protocol": "connectcoin-typed-v1"
    }
]
```

The command MUST return an array, possibly empty, of entries that contain at least a `fingerprint` field. A signer used by a wallet MUST also return the exact string `"protocol": "connectcoin-typed-v1"`. Signers without it remain visible through `enumeratesigners` with `connectcoin_compatible: false`, but wallet creation and signing reject them.

If present, the optional `model` field is used as the device name in the `enumeratesigners` RPC result.

A future extension could add an optional return field `reachable`, in case `<cmd>` knows a signer exists but can't currently reach it.

### `signtx` (required)

`signtx` indicates a PSBT signing-request, followed by Base64-encoded PSBT. Quotes are optional.

This command reads request `<base64-encoded psbt>` and MUST return a JSON object. On success, it MUST contain `{"psbt": "<base64-encoded psbt>"}` updated to include any new signatures. On failure, it SHOULD contain `{"error": "<message>"}`. Presence of a key `error` with value `null` is not considered an error.

PSBT SHOULD include BIP32 derivations. The command SHOULD fail if none of the BIP32 derivations match a key owned by the device.

The command SHOULD fail if the user cancels.

The command MAY complain if `--chain` is set to a test-network, but any of the BIP32 derivation paths contain a coin type other than `1h` (and vice versa).

### `getdescriptors` (optional)

Usage:

```sh
<cmd> --fingerprint <fingerprint> --chain <name> getdescriptors --account <account>
```

Returns descriptors supported by the device. Example:

```sh
<cmd> --fingerprint 00000000 --chain main getdescriptors --account 0
```

```
{
  "receive": [
    "tr([00000000/86h/0h/0h]ccpub.../0/*)#4d8tq2ns"
  ],
  "internal": [
    "tr([00000000/86h/0h/0h]ccpub.../1/*)#d63h6jpt"
  ]
}
```

### `displayaddress` (optional)

Usage:
```sh
<cmd> --fingerprint <fingerprint> --chain <name> displayaddress --desc <descriptor>
```

Example, display the first type-1 receive address on Testnet:

```sh
<cmd> --fingerprint 00000000 --chain test displayaddress --desc "tr([00000000/86h/1h/0h]tcub..../0/0)"
```

The command MUST be able to figure out the address type from the descriptor.

The command MUST return an object containing `{"address": "[the address]"}`.
As a sanity check, for devices that support this, it SHOULD ask the device to derive the address.

If `<descriptor>` contains a master key fingerprint, the command MUST fail if it does not match the fingerprint known by the device.

If `<descriptor>` contains an xpub, the command MUST fail if it does not match the xpub known by the device.

The command MAY complain if `--chain` is set to a test-network, but the BIP32 coin-type is not `1h` (and vice versa).

## How ConnectCoin Core uses the Signer API

The `enumeratesigners` RPC calls `<cmd> enumerate`, skips duplicate entries with the same `fingerprint`, maps `model` to `name`, and maps the exact `connectcoin-typed-v1` advertisement to `connectcoin_compatible`.

Wallet operations that need a signer (`createwallet`, `walletdisplayaddress` and spending) also call `<cmd> enumerate` and fail unless exactly one compatible signer is found.

The `createwallet` RPC calls:

* `<cmd> --fingerprint 00000000 --chain <name> getdescriptors --account 0`

It ignores legacy Bitcoin descriptor families and imports only key-path-only `tr()` or `rawtr()` descriptors for ConnectCoin type-1 outputs. At least one receive and one internal descriptor are required.

The `walletdisplayaddress` RPC obtains the inferred descriptor for the provided address. It then calls `<cmd> --fingerprint 00000000 --chain <name> displayaddress --desc <descriptor>`.

For external-signer wallets, spending uses `send` or `sendall`, and fee-bumping uses `bumpfee`. ConnectCoin Core builds a PSBT, adds key origin information, checks whether any input key origin fingerprint matches the signer, calls `<cmd> --stdin --fingerprint 00000000 --chain <name>`, and sends `signtx <psbt>` over stdin. If signatures are sufficient, it finalizes the transaction and, for broadcasting RPCs, broadcasts it. If signing cannot complete, the call fails with an error. For manual fee-bumping, use `psbtbumpfee` to obtain a PSBT for signing.
