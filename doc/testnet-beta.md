# Testnet-only beta and unlaunched mainnet

ConnectCoin mainnet has no operational genesis block. Its consensus and address
parameters remain available to libraries and tests, but daemon and GUI startup
reject default/mainnet selection with a clear error. The chainstate constructor
also rejects missing-genesis parameters, including through the Kernel API,
before opening the block database. Existing mainnet data is not migrated,
adopted, or erased; `-reindex` cannot enable the network.

Use `-testnet4` for the public-test-network profile or `-regtest` for local
tests. The name `testnet4` is an inherited internal identifier, not the fourth
public ConnectCoin beta. The existing testnet3, testnet4, signet and regtest
genesis blocks, network identifiers and consensus rules are unchanged.

The default chain selector remains `main` for compatibility with offline tools
and configuration parsing. Node startup without an explicit test-network
selection therefore fails, rather than silently choosing a different wallet or
data directory. A beta launcher must explicitly select `-testnet4`.

Testnet4 uses P2P port 48179 and RPC port 48178. RPC should remain private; expose
only the P2P service to testers. There are currently no built-in public seeds.
A public beta additionally needs reachable peers, bootstrap discovery, ongoing
RandomX mining, and distribution of test coins. Merely selecting `-testnet4`
does not create those services. No test balances are promised mainnet conversion.

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
