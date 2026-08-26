# ConnectCoin public identity

This file is the canonical inventory of identifiers that must be reviewed and
frozen before a public ConnectCoin network is launched. Values may still change
while the project is explicitly in development. Registry and name-conflict
checks below were last refreshed on 2026-08-25.

## Pre-launch blockers

The current brand is suitable for local development, but is **not cleared for a
public launch**:

- “Connect Coin” has already been used by the XCON cryptocurrency/token.
- A separate 2025 GitHub repository describes a Connects Health USA
  `Connectcoin` project and `CONN` token.
- A United States application for the `CONNECT COIN` mark covered
  cryptocurrency exchange, payment, storage, and key-generation services. The
  application is reported as abandoned, but that does not establish that the
  name is free to use in every jurisdiction.
- `ConnectCoin` has also been used as the name of a Motorola-branded Bluetooth
  accessory and application.
- `COIN CONNECT` is a registered US mark owned by DraftKings for online casino
  and games-of-chance services. Its scope is different, but it is another close
  mark that a professional search must evaluate.
- `CC` is already assigned to Canton Coin in SLIP-0044 and is actively used as
  that network's exchange ticker.
- `connectcoin.com` was already registered and resolving when checked. GitHub
  accounts using `Connectcoin` and `Connectcoinnode` also already existed.
- No project-owned public source repository, issue tracker, security contact,
  release-signing identity, DNS seed domain, or package-publishing account has
  been assigned yet. The development checkout retains Bitcoin Core as its
  `origin` fetch source, while its local push URL is deliberately set to the
  non-resolving `connectcoin.invalid` domain. Fresh clones do not inherit that
  local protection and must verify `git remote -v` before any push.
  Development binaries therefore state that no project-owned public source URL
  is configured instead of advertising Bitcoin Core's repository as their own.

Development application identifiers use the reserved domain
`connectcoin.invalid` and the reverse prefix `invalid.connectcoin`. Replace them
only after the project controls a stable domain; do not use `connectcoin.org`,
which is already registered and is not identified as project-owned.

Before any public mainnet, exchange listing, fundraising, or distribution,
perform a professional trademark and naming search in the intended countries,
check domains and software-package namespaces, and either clear or replace the
name and ticker. The exact package name returned no package in npm, PyPI,
crates.io, or NuGet during this snapshot, but absence is neither a reservation
nor legal clearance. A web and registry search is evidence of conflicts, not a
substitute for legal review.

## User-facing identity

| Item | ConnectCoin value |
| --- | --- |
| Product | ConnectCoin Core |
| Executable prefix | `connectcoin` / `connectcoind` |
| Data directory | `ConnectCoin` on Windows/macOS, `.connectcoin` on Unix |
| Configuration | `connectcoin.conf` |
| Payment URI | `connectcoin:` |
| Display unit | `CC` |
| Atomic unit | `connect` (`con`) |
| Signed-message header | `ConnectCoin Signed Message:\n` |
| P2P user agent | `/ConnectCoin:<version>/` |

`CC` is a development ticker, not a claim of global uniqueness. The SLIP-0044
registry already uses the symbol `CC` for Canton Coin at coin type 6767, and
Canton's own current whitepaper identifies `CC` as its abbreviation.
ConnectCoin has no registered BIP44/SLIP-0044 coin type yet. A unique ticker and
coin type must be selected or registered before public wallet interoperability
is advertised. The atomic-unit labels `connect` and `con` are project-specific
display names and have no standards registration.

## Address and key encodings

| Network | P2PKH | P2SH | WIF | HD public | HD private | Segwit HRP |
| --- | ---: | ---: | ---: | --- | --- | --- |
| Mainnet | 28 (`C…`) | 87 (`c…`) | 80 (`C…` compressed) | `ccpub…` (`a7c73fd9`) | `ccprv…` (`a7c73b9f`) | `cc` |
| Testnet3/testnet4/signet | 65 (`T…`) | 127 (`t…`) | 178 (`T…` compressed) | `tcub…` (`04313a97`) | `tcpr…` (`04313316`) | `tcc` |
| Regtest | 65 (`T…`) | 127 (`t…`) | 178 (`T…` compressed) | `tcub…` (`04313a97`) | `tcpr…` (`04313316`) | `ccrt` |

The `cc` human-readable prefix does not currently appear in the official
SLIP-0173 registry. It is therefore provisional, not reserved. Submit a
SLIP-0173 registration before a public launch and re-check the registry at that
time.

The four HD version-byte values above do not appear in SLIP-0132. Their readable
prefixes were selected for this development fork, but they are not registered
or globally collision-free. Register them, or replace them with accepted values,
before claiming hardware-wallet or multi-wallet interoperability. Base58 P2PKH,
P2SH, and WIF version bytes also need an ecosystem-wide collision review; there
is no single authoritative registry that makes a choice safe merely because it
produces the desired first character.

The `connectcoin:` payment URI is implemented by the GUI but does not appear in
the IANA URI Schemes registry. It is a project-local scheme until registered;
external applications must not assume global ownership of it.

## Network separation

The development monetary policy caps individual monetary values at
`100,000,000 CC`. On mainnet, testnet3, testnet4, and signet, the recurring
block subsidy begins at `100 CC` and halves every 450,000 blocks. Their separate
spendable genesis output allocates `10,000,000 CC` to the development fund. If
every available subsidy is claimed, integer rounding produces a maximum
mainnet issuance of
`99,999,899.9994150000 CC`. One CC is subdivided into `10,000,000,000`
connects, so wallet and RPC amounts use up to ten decimal places. Regtest keeps
its 150-block halving interval.

The largest atomic amount is `10^18` connects, which is greater than the largest
integer (`2^53 - 1`) represented exactly by an IEEE-754 binary64 value such as a
JavaScript `Number`. RPC integrations must parse returned amount tokens with an
arbitrary-precision decimal type and should submit amounts as decimal strings;
converting monetary values through binary floating point can lose connects.

The public networks use a valid compressed P2PK development-fund key whose
private key is deliberately absent from the source tree. Regtest alone uses a
deterministic key with published private material so automated tests can spend
its genesis allocation. The public-network private key requires offline backup
and production-grade custody before launch; changing it requires regenerating
the four public genesis blocks.

| Network | RPC port | P2P port | Onion bind | Message start | Genesis hash |
| --- | ---: | ---: | ---: | --- | --- |
| Mainnet | 48172 | 48173 | 48174 | `d9 51 a5 e2` | `0000004b461aae33a4be0ee95ae8461155f2c48130bc8dd521adb71ec0d3e9a2` |
| Testnet3 | 48175 | 48176 | 48177 | `03 84 8e 59` | `000001c906bb16924aaa92ab23ba23616d7916ecfdc3a256c060dbe9ead948f3` |
| Testnet4 | 48178 | 48179 | 48180 | `bb 51 f5 e7` | `000001218f2321c9ccd0f18fe8603865e9a97ea644d8ab62c434cc39928377f1` |
| Signet | 48181 | 48182 | 48183 | `54 d2 6f bd` (default `OP_TRUE` challenge) | `000000850a5c90845788b6f2688e27976da0ef181258378d39764f85baa64780` |
| Regtest | 48184 | 48185 | 48186 | `a5 4f c7 d5` | `197dd70fa5df793d1b9e4684f3c9608afcdae4b86f935c04e8187a48def347f6` |

Fixed seeds are intentionally absent during development. The inherited Bitcoin
peer snapshots were removed from `contrib/seeds`, and the generated seed header
is not included by the chain parameters. DNS seed hostnames and fixed seed
snapshots must only be added after ConnectCoin-controlled discovery
infrastructure exists. Regtest retains only the non-resolving
`dummySeed.invalid.` test-framework placeholder; it is not a bootstrap server.

All fifteen chosen RPC/P2P/onion-bind ports are in IANA's User Port range
(1024–49151). No entry from 48172 through 48186 appeared in the registry checked on the date
above. “No entry” means unassigned at that moment, not reserved for ConnectCoin.
Re-check all ports and request assignments before public
deployment. Test networks may also share one registered service name with
distinct documented ports if that is the final deployment design.

## Deliberately inherited internal identifiers

Source filenames, C++ types, translation contexts, BIP test vectors, copyright
notices, upstream links, and protocol domain separators such
as `Bitcoin seed` or `bitcoin_v2_shared_secret` may keep their upstream
spelling. Renaming those does not improve user-visible identity and can break
standards compatibility, attribution, build tooling, or reviewable history.
Installed executables, CMake targets, test-framework environment variables,
application metadata, RPC descriptions, network constants, addresses, paths,
CI outputs, developer tools, and ConnectCoin package artifacts must not use
inherited product names.

Project-owned include guards and the unique-name helper macro use the
`CONNECTCOIN_` prefix. Third-party subtree identifiers remain unchanged.

The experimental kernel API is a ConnectCoin-native, intentionally
ABI-incompatible fork. Its library, header, pkg-config file, CMake targets,
export macros, and C symbol prefix are named `libconnectcoinkernel`,
`connectcoinkernel.h`, `CONNECTCOINKERNEL_*`, and `cck_*`. Do not reintroduce
the inherited public `libbitcoinkernel`, `BITCOINKERNEL_*`, or `btck_*` names.

Translation catalogs are also inherited. New ConnectCoin source strings fall
back to English until a project-owned translation workflow is established; old
translated Bitcoin product strings are not considered approved ConnectCoin
branding.

The inherited previous-release downloader and compatibility fixtures remain
inactive reference material. Bitcoin Core release binaries are not valid
ConnectCoin compatibility targets because the networks have different genesis
blocks and identity constants. CI must not run those tests until ConnectCoin has
project-owned release artifacts and checksums.

## Registries

- [SLIP-0173 address-prefix registry](https://github.com/satoshilabs/slips/blob/master/slip-0173.md)
- [SLIP-0044 coin-type registry](https://github.com/satoshilabs/slips/blob/master/slip-0044.md)
- [SLIP-0132 HD version-byte registry](https://github.com/satoshilabs/slips/blob/master/slip-0132.md)
- [IANA URI Schemes registry](https://www.iana.org/assignments/uri-schemes/)
- [IANA Service Name and Port Number registry](https://www.iana.org/assignments/service-names-port-numbers/)
- [Prior `CONNECT COIN` US trademark application](https://trademarks.justia.com/888/02/connect-88802798.html)
- [Prior XCON asset listing](https://www.coinbase.com/price/connect-coin)
- [Connect Social filing describing its Connect Coin](https://www.sec.gov/Archives/edgar/data/1967788/000196778823000002/Connect_Social_Form_C.pdf)
- [Motorola/Binatone ConnectCoin product listing](https://binatoneglobal.com/end-of-life-products/)
- [Registered `COIN CONNECT` US mark](https://furm.com/trademarks/coin-connect-97293368)
- [Official Canton documentation identifying Canton Coin as `CC`](https://docs.canton.network/overview/understand/canton-coin)
- [Existing Connects Health USA `Connectcoin` repository](https://github.com/Connectshealthusa/connectcoin)
- [`connectcoin.com` RDAP record](https://rdap.verisign.com/com/v1/domain/CONNECTCOIN.COM)
