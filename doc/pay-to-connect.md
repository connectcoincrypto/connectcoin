# Pay-to-connect (P2C)

This document specifies ConnectCoin transaction output type `2`, named
`PAY_TO_CONNECT`. There is exactly one type-2 form: pay-to-domain. There is no
pay-to-domain-certificate mode and output type `3` is unassigned.

The purpose of P2C is to let an output be redeemed with a TLS 1.3
server-authenticated transcript bound to a claim and a specified DNS domain.
Verification is deterministic and does
not make a network connection: every handshake message and certificate needed
by consensus is supplied in the spending transaction witness.

## Output payload

The type-2 payload contains, in order:

| Field | Size | Meaning |
| --- | ---: | --- |
| `domain_length` | 1 byte | Number of bytes in `domain` |
| `domain` | variable | Canonical DNS name |
| `connection_work_target` | 32 bytes | Largest accepted P2C work hash |
| `root_certificates_version` | 4 bytes | Identifier of an immutable root bundle |
| `signature_algorithms_mask` | 1 byte | Allowed TLS CertificateVerify signature schemes |

The mandatory mask uses bit 0 for ECDSA P-256/SHA-256 (`1`), bit 1 for
`rsa_pss_rsae_sha256` (`2`), and bit 2 for `rsa_pss_pss_sha256` (`4`).
The default mask is `7` (all three); `6` permits both RSA schemes. Zero and
reserved bits are rejected. The byte follows the root version directly,
without a length prefix. Previous type-2 payloads without this byte have no
fallback decoder.

The domain is lower-case ASCII, contains only DNS LDH-label characters, has no
empty labels or trailing dot, and is at most 253 bytes. Wildcards, IP literals,
Unicode U-labels, underscores, and noncanonical spellings are rejected.

Version `1` is currently the only supported trusted-root bundle. Its source file
is `src/consensus/p2c_roots_v1.pem`. The bundle is immutable consensus data: an
update requires a new root version and an explicit consensus deployment.

## Redemption witness

A type-2 input has an empty `scriptSig` and exactly one witness element. That
element is at most 64 KiB and contains:

1. one byte with proof version `2`;
2. the complete raw TLS `ClientHello` handshake message;
3. the complete raw TLS `ServerHello` handshake message;
4. the complete raw TLS `EncryptedExtensions` handshake message;
5. the complete raw TLS `Certificate` handshake message; and
6. the complete raw TLS `CertificateVerify` handshake message.

Each raw handshake message includes its one-byte message type and three-byte
length header. TLS record headers, encrypted record framing, `Finished`, and
application data are not included. The Certificate message must carry the leaf
certificate followed by every server-supplied intermediate needed to build the
path. The trusted root is obtained locally from the output's root-bundle
version; it is not included in the proof.

The Certificate message is limited to 48 KiB, at most eight certificates, and
at most 16 KiB per certificate. The parser also applies individual limits to
the other handshake messages.

## Claim challenge

The exact 32-byte claim challenge is:

```
TaggedHash("ConnectCoin/P2C/claim/v1", txid || input_index)
```

It must appear verbatim in `ClientHello.random`. The transaction ID excludes
witness data, so adding the proof does not create a hash cycle. It does commit
the proof to the complete non-witness transaction and selected input. Any RBF
change that changes the transaction ID requires a new TLS connection proof.

The `getp2cchallenge` RPC returns the exact byte string to place in
`ClientHello.random`; callers must not reverse or reinterpret it as an integer.

## TLS 1.3 profile

Proof version 2 accepts this deliberately narrow profile:

- TLS 1.3 must be offered and selected.
- The selected cipher suite must be TLS_AES_128_GCM_SHA256 (`0x1301`) or
  TLS_CHACHA20_POLY1305_SHA256 (`0x1303`).
- `ClientHello` must contain exactly the output domain as SNI and must contain
  `supported_versions`, `signature_algorithms`, and `key_share`.
- X25519 and uncompressed secp256r1 are the supported key-share groups.
- HelloRetryRequest, PSK/resumption, early data, ECH, and compressed
  certificates are rejected.
- `CertificateVerify` may use ECDSA secp256r1 SHA-256 or RSA-PSS SHA-256. RSA
  leaf keys must be at least 2048 bits. The selected scheme must be permitted
  by the output's `signature_algorithms_mask`. RSAE schemes require an
  `rsaEncryption` leaf public key; PSS-PSS schemes require an `id-RSASSA-PSS`
  leaf public key whose restrictions permit SHA-256, MGF1-SHA-256 and a
  32-byte salt. Both RSA schemes require exactly that salt length.
- The leaf certificate must be valid for TLS server authentication and the
  output domain. Its chain must terminate at a root in the selected immutable
  bundle.

Certificate validity is evaluated at the previous block's median time past for
block validation and the current tip's median time past for mempool validation.
This keeps the result deterministic and independent of a node's wall clock.
Block-template assembly rechecks P2C inputs at the current tip median time so a
certificate that expires while its transaction remains in the mempool cannot
invalidate an otherwise valid mining template.

The SHA-256 TLS transcript used for `CertificateVerify` is the concatenation of
the first four raw handshake messages, through Certificate. Verification uses
the TLS 1.3 server CertificateVerify context string and the leaf public key.

## Connection work

The connection-work hash is:

```
TaggedHash(
    "ConnectCoin/P2C/work/v2",
    ClientHello || ServerHello || EncryptedExtensions ||
    Certificate
)
```

The proof is accepted only when this hash, interpreted with ConnectCoin's
normal 256-bit hash ordering, is less than or equal to the output's
`connection_work_target`.

The entire `CertificateVerify` message is excluded from the work hash: its
handshake header, signature scheme, length fields and signature bytes. It is
still mandatory in the witness and its signature must authenticate the first
four raw messages, including their handshake headers. Altering an equivalent
ECDSA signature encoding or replacing `s` with `n-s` therefore gives the same
work hash and no additional target attempts. Invalid signatures remain invalid.
The proof-version byte is not hashed either; the parser accepts only version 2
and the work tag provides version-specific domain separation.

For uniform independent candidate hashes and inclusive target `T`, success
probability is `(T + 1) / 2^256`. Interpreting candidate trials as connections
requires an independent server: an operator holding its signing key can
manufacture or selectively release authenticated responses locally. P2C does
not prove a counted number of physical connections, completed application
requests, or independent visitors.

Each P2C output is a normal UTXO and can be spent only once. There is no
remaining-claims counter and no consensus set of previously used connection
hashes.

## P2C mask v1 test-chain reset (September 9, 2026)

“P2C mask v1” identifies the network/output-layout reset that appends the
required one-byte signature-algorithms mask. It does not rename the proof
format: TLS proofs remain version 2 and the work tag remains
`ConnectCoin/P2C/work/v2`. Version 1 proofs are rejected. The claim challenge
tag `ConnectCoin/P2C/claim/v1` and immutable root bundle version 1 are also
unchanged; these identifiers describe different formats.

Testnet3, testnet4, signet and regtest have new genesis blocks and P2P message
starts. The change is active from their genesis, not a height-based reinterpretation
of the old chains. There is no conversion of old balances or automatic migration
of old block databases. Preserve wallets and backups, and use a fresh chain data
directory. All peer/seed nodes must update together. Mainnet remains unlaunched.
See [testnet-beta.md](testnet-beta.md) for reset identifiers and operator steps.

## RPC and command-line workflow

Create a transaction whose output object contains a `p2c` member with
`amount`, `domain`, `connection_work_target`, and `root_certificates_version`.
The optional `signature_algorithms_mask` defaults to `7`. The wallet
`sendtop2c` RPC also accepts this optional named argument; it is appended after
`verbose` for positional callers. Then:

```
connectcoin-cli getp2cchallenge "unsigned_transaction_hex" 0
```

Use the returned `clienthello_random` in an external TLS proof generator. Once
the complete version-2 proof is available, attach it without changing the
transaction ID:

```
connectcoin-cli setp2cproof "unsigned_transaction_hex" 0 "proof_hex"
connectcoin-cli testmempoolaccept '["witnessed_transaction_hex"]'
connectcoin-cli sendrawtransaction "witnessed_transaction_hex"
```

The offline transaction utility also supports:

```
connectcoin-tx outp2c=VALUE:DOMAIN:TARGET:ROOTS_VERSION[:SIGNATURE_ALGORITHMS_MASK]
connectcoin-tx p2cproof=INPUT_INDEX:PROOF
```

ConnectCoin Core provides parsing, validation, transaction construction,
challenge calculation and proof attachment. Its opt-in wallet claim worker
also captures TLS proofs; manual integrations may use an external generator.
Consensus validation itself never contacts the domain. See
[p2c-wallet.md](p2c-wallet.md) for the wallet workflow.

## Root-bundle provenance

Root bundle version 1 is the Mozilla CA set distributed by curl's CA Extract
service on 2026-08-13. The checked-in PEM file has SHA-256 digest:

```
f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
```

The certificate data retains its Mozilla source notice. P2C certificate and
CertificateVerify validation uses the hash-pinned Mbed TLS 3.6.7 dependency.
