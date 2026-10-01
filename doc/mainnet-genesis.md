# ConnectCoin mainnet genesis

The production mainnet genesis is fixed in `src/kernel/chainparams.cpp`.
It is separate from the retired development chain and every test network.
The daemon, GUI and CLI tools default to mainnet unless a command-line or
configuration option explicitly selects another network.

## Commitments

| Field | Value |
| --- | --- |
| Block ID (SHA256d) | `30a3a7543f593b6343873a16aeb61005dce0fe3f4169ab34039316b2a9bb373e` |
| RandomX v2 proof hash | `000016a94be04d8a4d9657f6d79466bfeb26798b02adaf4e50da26c123407def` |
| Merkle root / coinbase txid | `2ff1604a1a6ed04110972a78c808d7b967f8f3d6ee754fcf33bded563edc2c8c` |
| Timestamp | `1790872995` (`2026-10-01 16:43:15 UTC`) |
| Header / coinbase version | `1` / `1` |
| Nonce | `215364` |
| Compact target (`nBits`) | `0x1e333300` |
| Target | `0000333300000000000000000000000000000000000000000000000000000000` |
| RandomX bootstrap key (uint256 display order) | `d91b262aecaac2c4868b2cbe1563538f107c33fbee8c5d373bdaa8e551567fe5` |

The block ID and proof-of-work hash are different: the former uses SHA256d,
while proof of work uses RandomX v2 on the 80-byte header. The bootstrap key is
passed to RandomX in Core's uint256 byte order, not as ASCII hex.

The exact coinbase headline is:

```text
Cloudflare 01/Oct/2026 Support for modern cryptographic algorithms in Workers
```

It refers to Cloudflare's October 1, 2026 announcement,
[Support for modern cryptographic algorithms in Workers](https://blog.cloudflare.com/workers-ml-kem-ml-dsa-support/).
The complete coinbase scriptSig is 86 bytes, within the 100-byte consensus limit.

## Allocations

Each CONN contains 10,000,000,000 connects. Each allocation below is therefore
50,000,000,000,000,000 connects; the total coinbase value is 10,000,000 CONN.
Output order is part of the genesis commitment and must not be changed.

| Output | Allocation | CONN | Mainnet address |
| --- | --- | ---: | --- |
| 0 | `pers` combined | 5,000,000 | `cc1p9xntgynqa5j7xqvarq3kry40ankfag9hxy5r0wk0sx3ugrtmyq6qty6p8u` |
| 1 | `ment` combined | 5,000,000 | `cc1p9jp626zjnd2ueylz6gr4fure7crj6aav3qnxmxc9fje7p6f20pzsmlvv0j` |

These are native typed P2PK outputs, type `1`, using the following x-only
MuSig2 aggregate keys directly, without a Taproot tweak:

```text
pers: 29a6b41260ed25e3019d18236192afecec9ea0b7312837bacf81a3c40d7b2034
ment: 2c83a568529b55cc93e2d20754f079f6072d77ac88266d9b054cb3e0e92a7845
```

Both outputs enter the UTXO set at height zero. They remain subject to the
normal 100-block coinbase maturity: the earliest block that can spend them is
height 100. No private keys or secret MuSig nonces are needed or included to
create this genesis allocation.

## Initial difficulty

The reference was the observed testnet4 tip at height 44302, block
`9646d3e2ce70e4f87064f81a7c31f2970cd085cf251ec20550228daa719c8eef`,
with `nBits=0x1f00ffff`. Dividing that target by exactly five yields
`0x1e333300`, so mainnet's initial expected work is five times that reference.
This is a fixed launch parameter, not a dependency on the future testnet tip.

The existing consensus `powLimit` remains `0x1f00ffff`; only the initial target
changes. Mainnet has no special minimum-difficulty blocks. Under the existing
10-second spacing and 86,400-second target timespan, the initial target applies
through height 8639. The first retarget occurs at height 8640, with the normal
factor-of-four adjustment bound. A delay between the genesis timestamp and
active mining contributes to that first adjustment.

## Reproduce and verify

`contrib/devtools/mainnet-genesis.json` contains the full public header,
coinbase, block serialization and expected hashes. The standalone helper
`contrib/devtools/mine-mainnet-genesis.cpp` uses Core's actual typed-transaction
serialization and bundled RandomX implementation. Its header documents the
build command. Verification does not mine or connect to any node:

```text
mine-mainnet-genesis --verify 215364
```

Without arguments the helper searches with six FAST threads and a best-effort
300-second deadline measured before dataset initialization. The deadline is
checked between hashes; dataset initialization and an in-flight hash cannot
be cancelled. A found solution is then checked with the LIGHT interpreter.
The launch nonce was additionally verified in
a separate LIGHT process and its SHA256d commitments independently decoded.

## Startup and network separation

- Mainnet: default, or explicit `-chain=main`; P2P 48173, RPC 48172,
  addresses beginning with `cc1`.
- Public beta: explicit `-testnet4` or `-chain=testnet4`; P2P 48179,
  RPC 48178, addresses beginning with `tcc1`. Existing explicit testnet
  configurations continue selecting testnet; the application does not rewrite them.
- Keep RPC private. Mainnet's built-in DNS/DDNS bootstrap hostnames are
  `connectcoin2.com`, `connectcoin3.com`, `connectcoin4.com` and
  `dememzea.tplinkdns.com`; testnet4 uses only `connectcoin1.com`. Bootstrap
  nodes must run the matching network on its native P2P port (48173 for
  mainnet, 48179 for testnet4). Fixed seed arrays remain empty on all networks.
  These source assignments do not deploy or reconfigure VPS nodes, DNS
  records or services. See the
  [operator policy](dnsseed-policy.md#current-bootstrap-hostnames).
- Mining remains off at every startup. `startmining` and the GUI miner support
  mainnet, testnet4 and regtest. They may intentionally bootstrap without peers;
  operators must avoid creating isolated competing mainnet chains.
- Use a fresh explicit `-datadir` for mainnet if the base directory contains
  an old development chain. Normal startup rejects a wrong genesis. Do not
  request `-reindex` as a cross-chain migration: reindexing can discard old
  indexes, and pruning may remove old block data. Keep independent backups.
- No test balances convert to mainnet, no wallets or chain data are moved, and
  fixing the genesis in source does not deploy or reconfigure VPS nodes,
  explorers, RPC services, or the separate ConnectWallet application.
