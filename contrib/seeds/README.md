# ConnectCoin seed generation

This directory contains the inherited tooling used to turn crawler output into
fixed seed arrays for the client. It does **not** currently contain production
ConnectCoin seed data, and `src/chainparamsseeds.h` is intentionally absent from
the build until project-owned seed data is generated and reviewed.

Until ConnectCoin operates project-owned crawlers and has enough independently
operated nodes to produce trustworthy snapshots, keep fixed seeds out of the
build. Testnet4 includes `connectcoin1.com`, `connectcoin2.com`, `connectcoin3.com`
and `dememzea.tplinkdns.com` as DNS/DDNS bootstrap hostnames;
their DNS records and P2P services must be operated separately as described in
[testnet-beta.md](../../doc/testnet-beta.md#testnet4-bootstrap-dns). Other test
deployments can bootstrap with explicit `-addnode` entries.
Do not populate this directory from Bitcoin Core DNS seeds, crawlers, or AS-map
snapshots and do not publish a release that implies those peers belong to
ConnectCoin.

Before adding fixed seeds, the release process must:

1. Collect separate crawler snapshots for each operational ConnectCoin network
   being enabled. Mainnet is unavailable and must not be advertised as operational.
2. Review the eligible service flags, minimum chain heights, freshness, network
   diversity, and operator ownership.
3. Run `makeseeds.py` against those ConnectCoin-only snapshots, supplying
   `--seeds`, `--asmap`, and `--minblocks`, and capture its standard output in the
   corresponding `nodes_*.txt` files. `generate-seeds.py` currently requires all
   four files: `nodes_main.txt`, `nodes_test.txt`, `nodes_testnet4.txt`, and
   `nodes_signet.txt`. Networks without reviewed peers must not be populated
   with substitute data; any empty inputs and their generated representation
   require review before integration.
4. `generate-seeds.py` writes the candidate header to standard output; it does
   not create or update `src/chainparamsseeds.h`. With the reviewed input files
   in `contrib/seeds`, capture a candidate separately, for example from the
   repository root:

   ```sh
   candidate_dir="$(mktemp -d)" || exit 1
   python3 contrib/seeds/generate-seeds.py contrib/seeds > "$candidate_dir/chainparamsseeds.h"
   ```

   Check that generation succeeded and review the candidate before copying or
   integrating it. The current chain parameters still clear their fixed seed
   arrays; generating a header alone does not enable peer discovery from it.
   Adding the header and connecting reviewed arrays to the intended networks
   requires a separate code review.
5. After integration, test first-start peer discovery from a clean data directory
   on each operational network being enabled. Do not attempt mainnet bootstrap
   while mainnet has no launch genesis.

`PATTERN_AGENT` in `makeseeds.py` must be reviewed for the ConnectCoin release
being built. The minimum acceptable chain height has no baked-in default:
`makeseeds.py` requires an explicit `--minblocks` value so an inherited or stale
height cannot silently filter the wrong network. The scripts are tooling, not a
source of authoritative peer data.
