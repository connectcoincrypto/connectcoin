# ConnectCoin seed generation

This directory contains the inherited tooling used to turn crawler output into
fixed seed arrays for the client. It does **not** currently contain production
ConnectCoin seed data, and `src/chainparamsseeds.h` is intentionally absent from
the build until project-owned seed data is generated and reviewed.

The Bitcoin Core `nodes_*.txt` lists and the instructions that downloaded data
from Bitcoin crawlers were deliberately removed during the ConnectCoin rebrand.
Reusing them would make a fresh ConnectCoin node attempt to discover Bitcoin
peers, which are on an incompatible network.

Until ConnectCoin operates project-owned crawlers and has enough independently
operated nodes to produce trustworthy snapshots, keep fixed seeds out of the
build and bootstrap test deployments with explicit `-addnode` entries.
Do not populate this directory from Bitcoin Core DNS seeds, crawlers, or AS-map
snapshots and do not publish a release that implies those peers belong to
ConnectCoin.

When project-owned seed infrastructure exists, the release process must:

1. Collect separate crawler snapshots for mainnet, testnet, testnet4, and signet.
2. Review the eligible service flags, minimum chain heights, freshness, network
   diversity, and operator ownership.
3. Run `makeseeds.py` against those ConnectCoin-only snapshots to create the
   corresponding `nodes_*.txt` files.
4. Run `generate-seeds.py .` and review the resulting
   `src/chainparamsseeds.h` diff before committing it.
5. Test first-start peer discovery from a clean data directory on every network.

`PATTERN_AGENT` in `makeseeds.py` must be reviewed for the ConnectCoin release
being built. The minimum acceptable chain height has no baked-in default:
`makeseeds.py` requires an explicit `--minblocks` value so an inherited or stale
height cannot silently filter the wrong network. The scripts are tooling, not a
source of authoritative peer data.
