# ConnectCoin typed transaction outputs

ConnectCoin is experimenting with a typed UTXO format that removes Script from
the transaction-output wire format. This is a hard fork from Bitcoin's
transaction format and is not backward-compatible with Bitcoin blocks,
transactions, UTXO snapshots, wallets, or signing hardware.

## Consensus wire format

Every transaction output is serialized as:

| Field | Size | Encoding |
| --- | ---: | --- |
| `nValue` | 8 bytes | signed little-endian atomic-unit amount |
| `type` | 1 byte | unsigned output-type identifier |
| payload | type-specific | defined by the selected type |

Type `0` is invalid and has no payload. It exists only so null objects and
malformed transactions can be parsed and rejected. Unknown types are rejected
during deserialization; they must not be interpreted as Script or skipped.

### Type 1: P2PK

The payload is exactly one fully valid 32-byte secp256k1 x-only public key. A
type-1 output is therefore exactly 41 bytes. It is spent with:

- an empty `scriptSig`;
- a witness stack containing exactly one 64-byte BIP340 Schnorr signature; and
- the BIP341-style transaction signature hash with `SIGHASH_DEFAULT`.

Consensus verifies the signature directly against the output key. It does not
execute `scriptPubKey`, Tapscript, redeem scripts, or witness scripts. Each
non-coinbase input consequently contributes exactly one signature operation.

The inherited wallet represents the x-only key as a Bech32m witness-v1 address
and uses its `tr(KEY)` key-path machinery to hold and sign with the corresponding
key. That representation is a wallet compatibility layer, not the consensus
output serialization. P2PKH, P2SH, P2WPKH, P2WSH, multisig, Script trees,
`OP_RETURN`, and arbitrary raw scripts are not valid output types.

### Type 2: PAY_TO_CONNECT

Type `2` has exactly one payload form. It pays for proving a fresh TLS 1.3
connection to a canonical DNS domain. Its payload is:

| Field | Size | Meaning |
| --- | ---: | --- |
| `domain_length` | 1 byte | Number of bytes in `domain` |
| `domain` | variable | Lower-case ASCII DNS LDH labels, without a trailing dot |
| `connection_work_target` | 32 bytes | Maximum accepted connection-work hash |
| `root_certificates_version` | 4 bytes | Immutable trusted-root bundle identifier |

There is no mode byte and no certificate-specific output form. The leaf
certificate and every intermediate sent by the server appear in the redemption
proof, where consensus validates the chain, domain, validity time, TLS
CertificateVerify signature, claim challenge, and connection-work target.

A type-2 spend has an empty `scriptSig` and exactly one witness element: the
versioned P2C proof, limited to 64 KiB. See [pay-to-connect.md](pay-to-connect.md)
for the complete proof profile.

## Internal compatibility view

`CTxOut::scriptPubKey` remains temporarily available in memory because wallet,
descriptor, PSBT, GUI, and RPC code use Bitcoin's destination abstraction. For
a valid type-1 output it is deterministically reconstructed as `OP_1 <32-byte
key>`. The canonical type and public key are stored separately, and only those
canonical fields are serialized or compared. Assigning any other compatibility
script produces invalid type `0`; its script bytes never enter consensus
serialization. Serializing a type-1 output also verifies that this compatibility
view still exactly matches the canonical key, so direct mutation cannot silently
change or desynchronize the consensus payload.

For type `2`, the compatibility view is a deterministic internal byte sequence
beginning with `OP_2`, followed by the domain length, domain, target, and root
bundle version. It exists only for inherited interfaces. It is neither the
consensus wire payload nor an executable Script.

## Coinbase witness commitment

Data outputs are not available, so the witness commitment is stored as a
canonical 36-byte push in the coinbase input metadata (`scriptSig`): the four
bytes `aa21a9ed` followed by the 32-byte commitment. If multiple canonical
markers are present, the last one is authoritative. The coinbase witness still
contains the single 32-byte reserved value.

`getblocktemplate` clients must declare the `typedoutputs` rule. Templates
advertise `!typedoutputs` and provide the canonical 37-byte push in both
`coinbaseaux.typedoutputs` and `default_witness_commitment`. The mining IPC
interface includes the same marker in `script_sig_prefix`; clients preserve
that prefix and append pool names or extra nonces after it.

## Migration boundaries

- All network genesis blocks were regenerated for the typed wire format.
- Pre-typed transactions, blocks, undo data, and UTXO databases are invalid.
- `assumeutxo` snapshots are disabled until fresh typed-output commitments are
  generated from finalized chains.
- The inherited Script engine is still used to validate BIP325 signet block
  challenges. This does not make Script a transaction-output consensus
  feature. The bundled signet miner supports trivial challenges (including
  ConnectCoin's default `OP_TRUE`) that need no solution; nontrivial custom
  challenges are not supported because BIP325's signing PSBT requires an
  arbitrary Script output that the typed-output format cannot represent.
- Upstream unit vectors that embed Bitcoin's Script-based transaction wire
  format, PSBT fixtures, P2SH execution-cache tests, and witness-script swap
  fixtures are not registered as ConnectCoin consensus tests. The experimental
  Kernel test executable remains compile-checked but its inherited Bitcoin
  transaction/block corpus is likewise not registered with CTest. Native typed
  serialization, Schnorr authorization, mempool, mining, wallet, RPC, and
  validation tests replace that coverage.
- `test/functional/test_runner.py` names the remaining incompatible inherited
  functional suites and benchmark fixtures explicitly. They are excluded only
  when they require removed address/descriptor families, arbitrary Script
  payloads, exact variable-script transaction sizes, or pre-type-1 serialized
  fixtures. Keeping this list centralized makes the temporary coverage debt
  reviewable and prevents a red legacy test from being mistaken for native
  typed-output coverage.
- Type `3` and all higher identifiers are unassigned and rejected. New output
  types require an explicit consensus specification and deployment decision.
