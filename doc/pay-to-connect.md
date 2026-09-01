# Pay-to-connect (P2C)

This document specifies ConnectCoin transaction output type `2`, named
`PAY_TO_CONNECT`. There is exactly one type-2 form: pay-to-domain. There is no
pay-to-domain-certificate mode and output type `3` is unassigned.

The purpose of P2C is to let an output be redeemed by proving a fresh TLS 1.3
connection to a specified DNS domain. Verification is deterministic and does
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

The domain is lower-case ASCII, contains only DNS LDH-label characters, has no
empty labels or trailing dot, and is at most 253 bytes. Wildcards, IP literals,
Unicode U-labels, underscores, and noncanonical spellings are rejected.

Version `1` is currently the only supported trusted-root bundle. Its source file
is `src/consensus/p2c_roots_v1.pem`. The bundle is immutable consensus data: an
update requires a new root version and an explicit consensus deployment.

## Redemption witness

A type-2 input has an empty `scriptSig` and exactly one witness element. That
element is at most 64 KiB and contains:

1. one byte with proof version `1`;
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

Proof version 1 accepts this deliberately narrow profile:

- TLS 1.3 must be offered and selected.
- The selected cipher suite must be TLS_AES_128_GCM_SHA256 (`0x1301`) or
  TLS_CHACHA20_POLY1305_SHA256 (`0x1303`).
- `ClientHello` must contain exactly the output domain as SNI and must contain
  `supported_versions`, `signature_algorithms`, and `key_share`.
- X25519 and uncompressed secp256r1 are the supported key-share groups.
- HelloRetryRequest, PSK/resumption, early data, ECH, and compressed
  certificates are rejected.
- `CertificateVerify` may use ECDSA secp256r1 SHA-256 or RSA-PSS SHA-256. RSA
  leaf keys must be at least 2048 bits.
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
    "ConnectCoin/P2C/work/v1",
    ClientHello || ServerHello || EncryptedExtensions ||
    Certificate || CertificateVerify
)
```

The proof is accepted only when this hash, interpreted with ConnectCoin's
normal 256-bit hash ordering, is less than or equal to the output's
`connection_work_target`. A stricter target statistically requires more fresh
connections before finding an acceptable transcript.

Each P2C output is a normal UTXO and can be spent only once. There is no
remaining-claims counter and no consensus set of previously used connection
hashes.

## RPC and command-line workflow

Create a transaction whose output object contains a `p2c` member with
`domain`, `connection_work_target`, and `root_certificates_version`. Then:

```
connectcoin-cli getp2cchallenge "unsigned_transaction_hex" 0
```

Use the returned `clienthello_random` in an external TLS proof generator. Once
the complete version-1 proof is available, attach it without changing the
transaction ID:

```
connectcoin-cli setp2cproof "unsigned_transaction_hex" 0 "proof_hex"
connectcoin-cli testmempoolaccept '["witnessed_transaction_hex"]'
connectcoin-cli sendrawtransaction "witnessed_transaction_hex"
```

The offline transaction utility also supports:

```
connectcoin-tx outp2c=VALUE:DOMAIN:TARGET:ROOTS_VERSION
connectcoin-tx p2cproof=INPUT_INDEX:PROOF
```

ConnectCoin Core currently provides canonical parsing, validation, transaction
construction, challenge calculation, and proof attachment. Network capture and
generation of the TLS proof are external to the node for now.

## Root-bundle provenance

Root bundle version 1 is the Mozilla CA set distributed by curl's CA Extract
service on 2026-08-13. The checked-in PEM file has SHA-256 digest:

```
f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
```

The certificate data retains its Mozilla source notice. P2C certificate and
CertificateVerify validation uses the hash-pinned Mbed TLS 3.6.7 dependency.
