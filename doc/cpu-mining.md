# CPU mining

ConnectCoin Core includes an optional continuous RandomX v2 CPU miner for
mainnet, `-testnet4` and `-regtest`. It is **off on every startup**. This is a basic
solo miner, not a pool/Stratum client. Signet mining requires its additional
challenge and is not supported by this miner.

## Wallet

Open **Mining** (pickaxe icon, **Alt+6**), choose CPU threads, and start.
Leave the reward address empty to generate a receiving address in the selected
wallet automatically. Optionally paste a type-1 P2PK address for this network
to pay elsewhere, or use the button to generate and display a wallet address.
The miner is shared by the whole node, including all open wallets. Changing
tabs or closing a wallet does not stop it or change its reward address.
The displayed active address is authoritative. Stop mining before changing it.
An external reward address does not need a loaded or unlocked wallet.
The Mining tab remains available with no wallets loaded, including after
closing the last wallet; generating a new wallet address requires an open wallet.

Desktop pop-up notifications (including received mining rewards) are disabled
by default. To enable them, open **Settings > Options > Display > Enable pop-up
notifications**. The checkbox takes effect after pressing **OK**, without a
restart, and is saved with the GUI preferences. Alternatively, set
`popupnotifications=1` in `connectcoin.conf` (or pass `-popupnotifications=1`)
and restart the GUI. Use `0` to disable them. An explicit configuration or
command-line value overrides the checkbox. This does not hide modal errors or
confirmation dialogs, and does not change transaction processing or logging.

The node continues validating blocks and transactions while mining. Choose
fewer threads than logical CPUs to leave processing capacity for validation
and other applications. The selectable range is 1 through 1024, independent of
the logical CPU count, with 1 as the conservative default. The wallet warns
when the selection exceeds the detected logical CPU count, but does not block
it. More software threads do not create more CPU cores: excessive counts can
reduce hashrate and node responsiveness, and consume more memory. Each active
RandomX VM needs additional working memory even though the dataset is shared.
`getcpumininginfo` reports the allowed ceiling as `max_threads` and the detected
hardware count separately as `logical_cpus`. Resource allocation failures are
reported in the miner's `error` field; 1024 is a ceiling, not a guarantee that
every machine can run that many workers.

## Daemon / RPC

With a node running on mainnet, the default target is the selected wallet:

```sh
connectcoin-cli -rpcwallet=YOUR_WALLET startmining "" 2
connectcoin-cli getcpumininginfo
connectcoin-cli stopmining
```

Omitting `address` also selects the wallet. With exactly one loaded wallet,
`-rpcwallet` is optional; with several, choose one explicitly. To pay an external
address, use `startmining "YOUR_P2PK_ADDRESS" 2`; this works even without
wallet support or a loaded wallet. Without a wallet an address is required.

Use `-testnet4` for the public test network or `-regtest` for a local test chain,
on both the node and CLI. Mining
rewards obey the same subsidy, block-weight penalty, and coinbase maturity
rules as blocks mined externally. Accepted block counts are not a balance:
blocks can become stale after acceptance.

Stopping is asynchronous: workers finish their current hash, and an in-flight
block submission can finish. Background RandomX dataset construction may
continue after mining stops; hashing does not wait for that construction.
Wait for `running: false` before
starting another session. Errors and progress are in `getcpumininginfo`.
Session counters reset at each start. Hashrate is a recent measurement, not a
network estimate; it includes initialization overhead.

## Resource use and chain changes

The miner shares ready key-specific RandomX contexts with validation. With
FAST enabled (the default on 64-bit builds), only active-chain preparation or
explicit local mining requests can initialize a full dataset (roughly 2 GiB).
At most two FAST cache entries/builds are admitted, for epoch transitions.
Hash requests never initiate FAST construction, wait for a pending dataset,
or change FAST eviction order. Unprepared historical/alternative-chain keys
and keys whose datasets are still building use consensus-equivalent LIGHT,
with no automatic promotion based on incoming headers or claimed chainwork.
Cold LIGHT initialization is serialized in a separate, single-entry cache
(256 MiB); concurrent hashes can retain an evicted context until they finish.
Long forks may therefore take longer to verify. Configuring
`-randomxfast=0` also applies to mining; LIGHT saves memory but is slower.
Memory/JIT initialization failures are reported or follow the existing
consensus-equivalent fallback to LIGHT.

The Mining page and `getcpumininginfo.randomx_dataset` report a coordinator
snapshot for the dataset of this mining session's most recently prepared key:

- `not_started`: this session has not prepared a key (also while initially waiting for headers).
- `preparing`: no ready FAST dataset is available yet; hashes can temporarily use LIGHT.
- `huge_pages`: the dataset's large-page allocation path succeeded.
- `regular_pages`: the dataset allocation fell back to regular pages.
- `light_fallback`: FAST initialization failed and the context fell back to LIGHT.
- `disabled`: FAST is disabled with `-randomxfast=0`.
- `unavailable`: mock or deterministic test hashing does not use this dataset.

This describes only the shared dataset, not the separate RandomX cache, VM
scratchpads, or JIT code pages. It reports the allocation API's result, not an
independent measurement of physical page sizes; some operating systems treat
large-page requests as a preference. The snapshot is refreshed before workers
start and after each mining round, not for every hash. It may lag background
initialization, remains unchanged after stopping, and resets on the next start.
It is separate from fatal mining errors and does not change hash validity.

The GUI shows a non-modal advisory for `regular_pages` or `light_fallback`;
mining is not blocked. Huge Pages availability depends on operating-system
configuration, permissions and available memory. The Core node does not reserve
system pages, grant privileges, elevate itself, or change that configuration.
Packages provide separate, explicitly opt-in setup helpers for
[Windows](windows-msi.md) and Linux (see the packaged Huge Pages instructions).
Installing silently, repairing or uninstalling does not run those helpers.
The macOS package explains the platform-specific allocation/fallback behavior;
it does not offer an unsupported privilege or system-setting change.

JIT remains enabled when supported. Validation uses Secure JIT by default.
The CPU miner, RPC block generation and `connectcoin-util grind` request Secure
JIT off; the backend's mandatory protection still wins on macOS ARM64, OpenBSD
and NetBSD. Hashing threads use separate policy-specific VM pools while sharing
the dataset. Disabling Secure JIT allows writable/executable hash-VM code pages
where permitted, trading W^X hardening for performance without changing hashes
or consensus. Those pages still share the node process with validation and any
loaded wallets; separate VM pools are not process isolation. These are code
defaults, not a new command-line or configuration-file option.

Workers have disjoint nonce sequences. A fresh coinbase extraNonce changes
the merkle root after a template refresh or nonce exhaustion, without removing
the height or witness commitment. The full block is not copied per thread.
The coordinator checks the tip at most every 50 ms while hashing and renews
the template at least every five seconds, updating time, difficulty, and
mempool selection. One hash/initialization or template construction can take
longer than this polling interval. Known headers ahead of the active chain
pause mining, but a fresh chain can bootstrap without peers. Mining without
peers can build an isolated fork; connect to the intended network before
relying on accepted blocks as network-confirmed rewards.

Do not expose the RPC interface to untrusted clients: it controls resource use
and the reward address. No mining state changes consensus rules.

## Hashing implementation

The integrated miner uses the ordinary single-hash RandomX API. Each hash
takes a VM from the shared policy-specific pool and returns it when finished.
There is no batching, exclusive per-worker VM retention, or manual CPU-affinity
policy; the operating system schedules the workers.

A hash request selects an already prepared FAST context when available, or
uses LIGHT without starting or waiting for FAST construction. The target is
validated and decoded once per template, and the 80-byte block header uses a
fixed-size serialization buffer. These small implementation details do not
change hashes, mining difficulty, or consensus. Huge Pages allocation and its
status reporting remain unchanged.
