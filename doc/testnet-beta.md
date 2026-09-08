# Testnet-only beta and unlaunched mainnet

ConnectCoin mainnet has no operational genesis block. Its consensus and address
parameters remain available to libraries and tests, but daemon and GUI startup
reject explicit mainnet selection with a clear error. The chainstate constructor
also rejects missing-genesis parameters, including through the Kernel API,
before opening the block database. Existing mainnet data is not migrated,
adopted, or erased; `-reindex` cannot enable the network.

Use `-testnet4` for the public-test-network profile or `-regtest` for local
tests. The name `testnet4` is an inherited internal identifier, not the fourth
public ConnectCoin beta. Testnet4's genesis was replaced on September 7, 2026
as described below. Testnet3, signet and regtest genesis blocks are unchanged.

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
only the P2P service to testers. `connectcoin1.com` is the built-in testnet4 DNS
seed; no public seed is added to mainnet, testnet3, signet or regtest. Fixed seed
IP addresses remain empty. A public beta additionally needs reachable peers,
operational DNS, ongoing RandomX mining, and distribution of test coins. Merely
listing the hostname in the client does not deploy those services. No test
balances are promised mainnet conversion.

## Testnet4 bootstrap DNS

On a fresh start with no known peers, the client queries the seed automatically.
`-dnsseed=0` disables DNS seeding; `-connect` also disables it by default. Existing
peer discovery, proxy handling and connection limits are unchanged. A seed only
provides peer addresses: blocks and transactions still undergo normal validation.

The current discovery code requests A/AAAA records for `x9.connectcoin1.com`,
where `9` selects `NODE_NETWORK | NODE_WITNESS`. These records must point only to
reachable, non-pruned ConnectCoin testnet4 nodes on TCP port 48179. The client
also assumes BIP324 support for these filtered results, so advertised nodes must
support v2 transport. Do not return website/CDN addresses or peers from another
network. Publish AAAA only when incoming IPv6 connections actually work.

For the initial single-node deployment, DNS can be hosted by the domain's DNS
provider: point both `connectcoin1.com` and `x9.connectcoin1.com` to the seed node's
public IP, with a TTL of at least 60 seconds. Serve these as DNS-only records,
not through an HTTP reverse proxy. No wildcard record is needed; unsupported
service filters should not claim capabilities the node lacks.

If the filtered name has no addresses, or the client uses a name proxy, Core
falls back to connecting to `connectcoin1.com:48179` to request peer addresses.
Thus the base hostname must also resolve to a reachable testnet4 P2P node, not
just a DNS server. This fallback is an address-fetch connection, not a promise
of a permanent connection to that node.

Running the authoritative DNS service on the VPS itself is optional and is
separate from running `connectcoind`. Provider-hosted DNS does not require
opening port 53 on the node. A dedicated crawler/DNS seeder can replace the
static records later, returning checked testnet4 peers. See the
[operator policy](dnsseed-policy.md); one bootstrap operator is an initial
central dependency, not a substitute for independent seeds.

An optional [CPU miner](cpu-mining.md) is available from the wallet's Mining
tab or the `startmining` RPC. It supports testnet4 and regtest, is disabled at
each startup, and does not require a loaded wallet when given a reward address.

## Testnet4 genesis reset (September 7, 2026)

The beta genesis allocates `10,000,000 CC` to a newly generated, wallet-owned
type-1 public key:

- Public key: `2ef316afd6177619f68ecfc6521fc3fcbf7faa2b25273f6ddea7971fae0de144`
- Address: `tcc1p9me3dt7kzampna5welr9y87rljlhl23ty5nn7mw757t3ltsdu9zqu5cd3u`
- Genesis: `06a1a1f822fed4a412aedb19315f1e85c963ad9b3c10e88ff12626b4b1389115`
- Coinbase transaction / Merkle root: `c20a4d5c39a400dde2e7d9eaeedc4c5df22bb2f9d4f471369ee67aa40da3a683`
- Header time: `1788814378`; nonce: `60490`; difficulty bits: `0x1f00ffff`.

The header was mined with real RandomX v2. The private key is held in a local
wallet, not this repository. A wallet backup was restored and its ability to
sign for the public key was independently verified before adopting the genesis.
The allocation remains subject to the 100-block coinbase maturity rule.

This is a new chain, not a migration of old test balances. Old testnet4 block
databases cannot be reused. Stop the node and preserve its old `testnet4/`
directory separately before initializing the new chain; do not delete wallet
backups. Keep backups of the fund wallet outside Git and make an offline copy.
This testnet key must not be reused for the future mainnet allocation.

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
