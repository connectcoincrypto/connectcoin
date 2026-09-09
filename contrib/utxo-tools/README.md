# ConnectCoin UTXO snapshot conversion

Create a snapshot with a running node, then convert it without accessing a wallet:

```sh
connectcoin-cli dumptxoutset /absolute/path/utxos.dat latest
python3 contrib/utxo-tools/utxo_to_sqlite.py /absolute/path/utxos.dat /absolute/path/utxos.sqlite
```

The converter supports ConnectCoin's version-2 snapshot container with **typed
outputs**, not Bitcoin's compressed-script payloads. The table remains:

```sql
utxos(txid TEXT, vout INT, value INT, coinbase INT, height INT, scriptpubkey TEXT)
```

`value` is an integer number of connects (10,000,000,000 per CC). The
`scriptpubkey` column contains Core's deterministic compatibility representation:

- Type 1 (P2PK): `51 20` followed by the 32-byte x-only public key.
- Type 2 (P2C): `52`, the one-byte domain length, canonical ASCII domain,
  32-byte little-endian work target, and four-byte little-endian root version.

These prefixes are **not** stored in the snapshot's typed payload. In particular,
type 1 must not be interpreted as Bitcoin's compressed P2SH script tag.

`--spk=raw` stores the compatibility representation as a BLOB. `--txid=hex`
uses the RPC/display transaction ID; `--txid=raw` uses its serialized bytes;
`--txid=rawle` stores the reverse of those serialized bytes as a BLOB. All six
combinations are supported. Named-pipe input is supported on Unix.

The converter checks lengths, integer bounds, canonical encodings, supported
output types, P2PK keys and P2C payload structure. It does not establish snapshot
authenticity, chain membership or complete consensus validity. Compare its data
with your independently validated node before relying on an external snapshot.

A temporary database is written in the destination directory and published only
after the entire input has been consumed successfully. Existing output files are
never overwritten. Publication requires hard-link support, available on NTFS
and usual Unix filesystems such as ext4; an unsupported destination filesystem
produces an error rather than replacing an existing file. A terminated process
may leave a `.utxo-sqlite-*.tmp` temporary file, but not a partial final database.
