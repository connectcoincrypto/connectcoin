# CPU mining (testnet beta)

ConnectCoin Core includes an optional continuous RandomX v2 CPU miner for
`-testnet4` and `-regtest`. It is **off on every startup**. This is a basic solo
miner, not a pool/Stratum client. Mainnet remains unavailable; signet mining
requires its additional challenge and is not supported by this miner.

## Wallet

Open **Mining** (pickaxe icon, **Alt+6**), generate a reward address or paste a
type-1 P2PK address for the current network, choose CPU threads, and start.
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

With a node running on testnet4, use these commands (replace the address):

```sh
connectcoin-cli -testnet4 startmining "YOUR_TESTNET_P2PK_ADDRESS" 2
connectcoin-cli -testnet4 getcpumininginfo
connectcoin-cli -testnet4 stopmining
```

Use `-regtest` instead for a local test chain. No wallet RPC is needed. Mining
rewards obey the same subsidy, block-weight penalty, and coinbase maturity
rules as blocks mined externally. Accepted block counts are not a balance:
blocks can become stale after acceptance.

Stopping is asynchronous: workers finish their current hash, and an in-flight
block submission can finish. Initial RandomX dataset construction also must
finish before its waiting workers exit. Wait for `running: false` before
starting another session. Errors and progress are in `getcpumininginfo`.
Session counters reset at each start. Hashrate is a recent measurement, not a
network estimate; it includes initialization overhead.

## Resource use and chain changes

The miner uses the same key-specific RandomX context cache as validation:
FAST by default (roughly 2 GiB per dataset), with separate VMs for concurrent
hashes. Epoch transitions can retain the old and next datasets. Configuring
`-randomxfast=0` also applies to mining; LIGHT saves memory but is slower.
Memory/JIT initialization failures are reported or follow the existing
consensus-equivalent fallback to LIGHT.

Workers have disjoint nonce sequences. A fresh coinbase extraNonce changes
the merkle root after a template refresh or nonce exhaustion, without removing
the height or witness commitment. The full block is not copied per thread.
The coordinator checks the tip at most every 50 ms while hashing and renews
the template at least every five seconds, updating time, difficulty, and
mempool selection. One hash/initialization or template construction can take
longer than this polling interval. Known headers ahead of the active chain
pause mining, but a fresh isolated testnet can bootstrap without peers.

Do not expose the RPC interface to untrusted clients: it controls resource use
and the reward address. No mining state changes consensus rules.
