# PSBT Howto for ConnectCoin Core

ConnectCoin Core inherits its PSBT container, roles, and RPC interface from
Bitcoin Core and [BIP 174](https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki),
but the payload is a **ConnectCoin-specific, non-interoperable dialect**. In
particular, PSBTv0's global unsigned transaction and every
`non_witness_utxo` contain ConnectCoin's typed transaction-output wire format
(`amount + type + payload`), not a Bitcoin transaction. PSBTv2 fields may look
structurally familiar, but signatures use ConnectCoin's type-1 digest and
authorization rules.

The current encoding retains the inherited `psbt\xff` magic. That is not a
compatibility promise: Bitcoin PSBT software, hardware wallets, and generic
signers may reject the data or, worse, interpret compatibility fields and sign
the wrong digest. Only ConnectCoin-aware implementations may parse or sign
these PSBTs. A distinct magic or mandatory dialect marker should be introduced
before public interoperability is offered so unsupported tools fail closed.

The PSBT container and descriptor machinery are compatibility infrastructure;
the current consensus accepts only type-1 single-key outputs and one 64-byte
Schnorr witness signature per input. Multisig and Script-based examples below
are inherited reference material and do not produce valid ConnectCoin
transactions. See [typed-outputs.md](typed-outputs.md).

This document describes the overall workflow for producing signed transactions
through the use of PSBT, and the specific RPC commands used in typical
scenarios.

## PSBT in general

Within ConnectCoin-aware software, PSBT is a container for ConnectCoin
transactions that are not fully signed yet, together with relevant metadata to
help entities work towards signing them. The inherited workflow supports
cooperation between components, but Bitcoin hardware-wallet, multisig, and
CoinJoin examples are not claims of compatibility with the current type-1-only
protocol.

### Overall workflow

Overall, the construction of a fully signed ConnectCoin transaction goes through the
following steps:

- A **Creator** proposes a particular transaction to be created. They construct
  a PSBT that contains certain inputs and outputs, but no additional metadata.
- For each input, an **Updater** adds information about the UTXOs being spent by
  the transaction to the PSBT. They also add information about the scripts and
  public keys involved in each of the inputs (and possibly outputs) of the PSBT.
- **Signers** inspect the transaction and its metadata to decide whether they
  agree with the transaction. They can use amount information from the UTXOs
  to assess the values and fees involved. If they agree, they produce a
  partial signature for the inputs for which they have relevant key(s).
- A **Finalizer** is run for each input to convert the partial signatures and
  possibly script information into a final `scriptSig` and/or `scriptWitness`.
- An **Extractor** produces a valid ConnectCoin transaction (in network format)
  from a PSBT for which all inputs are finalized.

Generally, each of the above (excluding Creator and Extractor) will simply
add more and more data to a particular PSBT, until all inputs are fully signed.
In a naive workflow, they all have to operate sequentially, passing the PSBT
from one to the next, until the Extractor can convert it to a real transaction.
In order to permit parallel operation, **Combiners** can be employed which merge
metadata from different PSBTs for the same unsigned transaction.

The names above in bold are the names of the roles defined in BIP174. They're
useful in understanding the underlying steps, but in practice, software and
hardware implementations will typically implement multiple roles simultaneously.

## PSBT in ConnectCoin Core

### RPCs

- **`converttopsbt` (Creator)** is a utility RPC that converts an
  unsigned raw transaction to PSBT format. It ignores existing signatures.
- **`createpsbt` (Creator)** is a utility RPC that takes a list of inputs and
  outputs and converts them to a PSBT with no additional information. It is
  equivalent to calling `createrawtransaction` followed by `converttopsbt`.
- **`walletcreatefundedpsbt` (Creator, Updater)** is a wallet RPC that creates a
  PSBT with the specified inputs and outputs, adds additional inputs and change
  to it to balance it out, and adds relevant metadata. In particular, for inputs
  that the wallet knows about (counting towards its normal or watch-only
  balance), UTXO information will be added. For outputs and inputs with UTXO
  information present, key and script information will be added which the wallet
  knows about. It is equivalent to running `createrawtransaction`, followed by
  `fundrawtransaction`, and `converttopsbt`.
- **`walletprocesspsbt` (Updater, Signer, Finalizer)** is a wallet RPC that takes as
  input a PSBT, adds UTXO, key, and script data to inputs and outputs that miss
  it, and optionally signs inputs. Where possible it also finalizes the partial
  signatures.
- **`descriptorprocesspsbt` (Updater, Signer, Finalizer)** is a node RPC that takes
  as input a PSBT and a list of descriptors. It updates SegWit inputs with
  information available from the UTXO set and the mempool and signs the inputs using
  the provided descriptors. Where possible it also finalizes the partial signatures.
- **`utxoupdatepsbt` (Updater)** is a node RPC that takes a PSBT and updates it
  to include information available from the UTXO set (works only for SegWit
  inputs).
- **`finalizepsbt` (Finalizer, Extractor)** is a utility RPC that finalizes any
  partial signatures, and if all inputs are finalized, converts the result to a
  fully signed transaction which can be broadcast with `sendrawtransaction`.
- **`combinepsbt` (Combiner)** is a utility RPC that implements a Combiner. It
  can be used at any point in the workflow to merge information added to
  different versions of the same PSBT. In particular it is useful to combine the
  output of multiple Updaters or Signers.
- **`joinpsbts`** (Creator) is a utility RPC that joins multiple PSBTs together,
  concatenating the inputs and outputs. This can be used to construct CoinJoin
  transactions.
- **`decodepsbt`** is a diagnostic utility RPC which will show all information in
  a PSBT in human-readable form, as well as compute its eventual fee if known.
- **`analyzepsbt`** is a utility RPC that examines a PSBT and reports the
  current status of its inputs, the next step in the workflow if known, and if
  possible, computes the fee of the resulting transaction and estimates the
  final weight and feerate.


### Workflows

#### Multisig with multiple ConnectCoin Core instances

For a quick start see [Basic M-of-N multisig example using descriptor wallets and PSBTs](./descriptors.md#basic-multisig-example).
