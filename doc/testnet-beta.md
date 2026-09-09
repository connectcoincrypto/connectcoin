# Testnet-only beta and unlaunched mainnet

ConnectCoin mainnet has no operational genesis block. Its consensus and address
parameters remain available to libraries and tests, but daemon and GUI startup
reject explicit mainnet selection with a clear error. The chainstate constructor
also rejects missing-genesis parameters, including through the Kernel API,
before opening the block database. Existing mainnet data is not migrated,
adopted, or erased; `-reindex` cannot enable the network.

Use `-testnet4` for the public-test-network profile or `-regtest` for local
tests. The name `testnet4` is an inherited internal identifier, not the fourth
public ConnectCoin beta. All four test-chain genesis blocks and message starts
were reset for P2C v2 on September 9, 2026, as described below.

The daemon, GUI and command-line tools default to `testnet4` during beta testing,
even without a `connectcoin.conf` file. An explicit network selection in the
command line or configuration still takes precedence; `-chain=main` is still
rejected at node startup. To choose another network, use `-chain=<chain>` or its
positive selector such as `-regtest`, not just `-testnet4=0`. No configuration
file is generated or rewritten by this default. Wallets and chain data use the
existing `testnet4/` subdirectory; old mainnet files are not moved or loaded.
The Kernel API's separate mainnet default and explicit chain parameters are
unchanged.

Testnet4 uses P2P port 48179 and RPC port 48178. RPC should remain private; expose
only the P2P service to testers. The built-in testnet4 DNS/DDNS seeds are listed
below; no public seed is added to mainnet, testnet3, signet or regtest. Fixed seed
IP addresses remain empty. A public beta additionally needs reachable peers,
operational DNS, ongoing RandomX mining, and distribution of test coins. Merely
listing hostnames in the client does not deploy those services. No test
balances are promised mainnet conversion.

## Testnet4 bootstrap DNS

The built-in base hostnames are:

- `connectcoin1.com`
- `connectcoin2.com`
- `connectcoin3.com`
- `dememzea.tplinkdns.com`

On a fresh start with no known peers, the client queries the seeds automatically.
`-dnsseed=0` disables DNS seeding; `-connect` also disables it by default. Existing
peer discovery, proxy handling and connection limits are unchanged. A seed only
provides peer addresses: blocks and transactions still undergo normal validation.

The current discovery code requests A/AAAA records for `x9.<seed>`, for example
`x9.connectcoin1.com`,
where `9` selects `NODE_NETWORK | NODE_WITNESS`. These records must point only to
reachable, non-pruned ConnectCoin testnet4 nodes on TCP port 48179. The client
also assumes BIP324 support for these filtered results, so advertised nodes must
support v2 transport. Do not return website/CDN addresses or peers from another
network. Publish AAAA only when incoming IPv6 connections actually work.

For a static bootstrap deployment, DNS can be hosted by the domain's DNS
provider: point both the base hostname and its `x9.` subdomain to the corresponding
seed node's public IP, with a TTL of at least 60 seconds. Serve these as DNS-only records,
not through an HTTP reverse proxy. No wildcard record is needed; unsupported
service filters should not claim capabilities the node lacks.

If the filtered name has no addresses, or the client uses a name proxy, Core
falls back to connecting to `<seed>:48179` to request peer addresses.
Thus the base hostname must also resolve to a reachable testnet4 P2P node, not
just a DNS server. This fallback is an address-fetch connection, not a promise
of a permanent connection to that node.

A DDNS provider may not allow a nested record such as
`x9.dememzea.tplinkdns.com`. The base-hostname fallback still works in that case,
provided `dememzea.tplinkdns.com` resolves to a reachable testnet4 node on TCP
48179. Keep its dynamic address updated and allow incoming P2P connections;
DDNS alone does not bypass NAT or a firewall. Multiple names pointing to the
same node do not provide independent bootstrap redundancy.

Running the authoritative DNS service on the VPS itself is optional and is
separate from running `connectcoind`. Provider-hosted DNS does not require
opening port 53 on the node. A dedicated crawler/DNS seeder can replace the
static records later, returning checked testnet4 peers. See the
[operator policy](dnsseed-policy.md); one bootstrap operator is an initial
central dependency, not a substitute for independent seeds.

An optional [CPU miner](cpu-mining.md) is available from the wallet's Mining
tab or the `startmining` RPC. It supports testnet4 and regtest, is disabled at
each startup, and does not require a loaded wallet when given a reward address.

## P2C v2 genesis reset (September 9, 2026)

The testnet4 beta genesis allocates `10,000,000 CC` to the same wallet-owned
type-1 public key generated on September 7. No new private key is needed:

- Public key: `2ef316afd6177619f68ecfc6521fc3fcbf7faa2b25273f6ddea7971fae0de144`
- Address: `tcc1p9me3dt7kzampna5welr9y87rljlhl23ty5nn7mw757t3ltsdu9zqu5cd3u`
- Genesis: `38cae555fb78f44c31e7d6859d0476252b321dae8b6312afefe0a45fc3fd112a`
- Coinbase transaction / Merkle root: `e70bc6f9408b4997f2b8f4f227bddd122282ceb4cc5b58d326081ee411441d4e`
- Header time: `1788912001`; nonce: `199567`; difficulty bits: `0x1f00ffff`.
- Message start: `4e 3d 81 78`; P2P port remains `48179`.

The header was mined with real RandomX v2. The private key is held in a local
wallet, not this repository. The original key's ownership was verified during
the September 7 setup; this reset reuses its public key without accessing or
regenerating the private key.
The allocation remains subject to the 100-block coinbase maturity rule.

The other reset genesis parameters (all with the same 10,000,000 CC allocation
and their previous public keys) are:

| Chain | Header time | Nonce | Bits | Genesis hash |
| --- | ---: | ---: | --- | --- |
| Testnet3 | 1788912000 | 38388 | `1f00ffff` | `ca89051d3a1bcf96be2ed4943d347687af47b6fd0a155fc2b15ddcc103bd75af` |
| Signet | 1788912002 | 27113 | `1f00ffff` | `2a62fd84425bc1f6dce0343ec3f6c08b782d76df54d52e5e3b8153f5d27d94b4` |
| Regtest | 1296688602 | 26 | `207fffff` | `de48ff31cbff58a91ef359100fef13e6472f165e6f0410e52efcdacb1861f65a` |

Each coinbase message is `ConnectCoin <network> | P2C v2 | 2026-09-09`, where
`<network>` is `testnet3`, `testnet4`, `signet`, or `regtest`. Regtest deliberately
keeps its historical header clock so tests using historical mock times remain
valid; its coinbase, Merkle root, nonce and chain identity are new. The genesis
blocks use real RandomX, not test-only mock proof of work. The isolated historic
mainnet test fixture is unchanged and is not an operational network.

This is a new chain, not a migration of old test balances. Version-1 P2C proofs
are not accepted. See [pay-to-connect.md](pay-to-connect.md) for the v2 hash.

Wallet database identifiers are separate from P2P message starts and retain
their pre-reset values. Backups from the same test-network profile remain
recognizable without editing their SQLite headers. Other network profiles are
still rejected; signet wallets also remain specific to their configured
challenge. This compatibility preserves keys and wallet metadata, not the old
chain or its funds. Temporary wallet files created by unreleased v2 development
builds that used the new P2P magic as their database identifier are not accepted.
The wallet's saved chain locator is a separate safeguard: a different genesis
is still rejected by default, even when its database identifier matches. Only
an intentional reset restore should override that safeguard as described below.

Before upgrading an existing node:

1. Stop it normally and back up its wallets outside Git, including an offline
   copy of the fund wallet. Never delete a wallet to reset a chain.
2. Preserve the entire old data directory. Start the new binary using an
   explicitly separate, newly created `-datadir=<new-directory>`; do not reuse
   an old block index, chainstate, mempool, settings or peer database.
3. For this intentional reset only, start the new binary with
   `-datadir=<new-directory> -walletcrosschain=1`, restore the desired wallet
   backup and explicitly run `rescanblockchain 0` in that wallet's RPC console
   (unlock an encrypted wallet first). This scan includes the new genesis;
   do not assume that restoration at height zero scanned its allocation.
   The override permits the old genesis
   in the wallet's saved locator; it does not bypass wallet network/challenge
   identifiers or block validation. Do not edit the backup's database header.
   Keys remain usable, but old chain transactions and balances do not become
   new-chain funds. The testnet4 genesis key controls the new allocation.
   Stop the node normally after restoration/rescan, then restart in the new
   directory without `-walletcrosschain`. Do not save that override permanently
   in the configuration. Keep the original backup untouched.
4. Upgrade all VPS/seed nodes and peers to the same build and fresh chain before
   advertising them. DNS names and port 48179 stay the same; updating this code
   does not deploy anything to those machines.

Normal startup refuses a loaded block index with the wrong genesis. Reindexing
is not a supported migration of the old chain. The reset performs no automatic
data deletion or wallet conversion. This testnet key must not be reused for the
future mainnet allocation.

## Wallet fees on a new network

When fee estimation has insufficient transaction history and `fallbackfee` is
unset or zero, automatic wallet sends use the next block's economic minimum:
the subsidy weight cost per kvB plus 1,000 connects/kvB. This follows halvings
and reorgs, rather than keeping the initial subsidy's fee forever. Higher
mempool, relay or wallet minimums still apply; the maximum-fee safeguards are
unchanged. A nonzero `fallbackfee` still selects a fixed alternative when
estimation is unavailable, subject to the same minimums.

This applies to GUI sends, funding/send RPCs and manual P2C claim preparation.
No confirmation deadline is implied by the minimum rate. Explicit transaction
fee rates retain their existing behavior. Fee estimation RPCs still report
insufficient data when there is no estimate: the economic minimum is a wallet
policy, not a fabricated estimate.

## Tests without a production mainnet

`src/test/util/chainparams.cpp` supplies the retired development genesis only to
the `test_util` library. It is a historical test vector, not a launch commitment,
and is not linked into the daemon, GUI, or Kernel library. There is no runtime
switch or configuration option to install it. Tests of storage, subsidy,
retargeting, genesis spending and fuzzing retain mainnet rules with this fixture.
Production-factory tests separately assert that mainnet has no genesis and that
its chainstate cannot be initialized. The original 35 mainnet address RPC
vectors run in C++ against the fixture, including rejection of legacy output
types. HTTP RPC coverage runs on regtest and is restored to the default suite.

The eventual mainnet release must define and validate a new launch genesis.
The retired development chain and all beta chains remain separate from it.
