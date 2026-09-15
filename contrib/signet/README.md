Contents
========
This directory contains tools related to Signet, both for running a Signet yourself and for using one.

getcoins.py
===========

A script to call an explicitly selected ConnectCoin-compatible faucet. No
public ConnectCoin faucet is configured, and a Bitcoin Signet faucet cannot
fund ConnectCoin because the networks have different genesis blocks and address
encodings.

Usage with an explicit address: `getcoins.py [-h|--help] -f|--faucet=<ConnectCoin HTTPS faucet URL> -a|--addr=<ConnectCoin Signet Bech32m address> [-p|--password=<faucet password>] [--allow-insecure-localhost]`

* `--cmd` lets you customize the connectcoin-cli path. By default it will look for it in the PATH
* `--faucet` is required and selects a ConnectCoin-compatible HTTPS faucet; compatibility and trust must be verified by the operator
* `--addr` should be supplied with a ConnectCoin Signet type-1 (Bech32m) address; this path does not require `connectcoin-cli`. Although the parser permits omitting it, the current automatic address-generation path requests the unsupported `bech32` address type and fails. Generate the address separately with `connectcoin-cli -signet getnewaddress` and pass it explicitly.
* `--password` passes a password understood by a faucet you operate or explicitly trust
* `--allow-insecure-localhost` permits HTTP only for `localhost` or a loopback IP and is intended solely for local development

The script deliberately has no default service. Use regtest for local testing
when no project-owned Signet faucet is available. Redirects, remote captcha
rendering, unbounded responses, and non-loopback plain HTTP are deliberately not
supported.

miner
=====

ConnectCoin's development Signet defaults to the permissionless `OP_TRUE`
challenge. Custom challenges must also be trivial truthy scripts requiring no
scriptSig or witness; signature-protected BIP325 challenges are not supported by
the current typed-output format. Blocks still require RandomX proof of work.

The `calibrate` subcommand estimates a difficulty target for your hardware, for example:

    MINER="./contrib/signet/miner"
    GRIND="./build/bin/connectcoin-util grind"
    $MINER calibrate --grind-cmd="$GRIND"

It defaults to estimating an nbits value resulting in 25s average time to find a block, but the --seconds parameter can be used to pick a different target, or the --nbits parameter can be used to estimate how long it will take for a given difficulty. Results depend on the hardware; this estimate does not change the network's consensus difficulty.

To mine the first block in your custom chain, you can run:

    CLI="./build/bin/connectcoin-cli -conf=mysignet.conf"
    ADDR=$($CLI -signet getnewaddress)
    NBITS="<nbits reported by calibrate>"
    $MINER --cli="$CLI" generate --grind-cmd="$GRIND" --address="$ADDR" --nbits="$NBITS"

This requests a single block with a backdated timestamp. The first-block
backdating and subsequent scheduling use the inherited interval described below.

Adding the --ongoing parameter keeps the miner running. The script uses --nbits
to choose its scheduling target, but always mines the difficulty supplied by the
node's block template.

    $MINER --cli="$CLI" generate --grind-cmd="$GRIND" --address="$ADDR" --nbits="$NBITS" --ongoing

**Scheduling limitation:** the script still uses the upstream interval
`600 * 2016 / 2015` seconds, while ConnectCoin Signet targets 10-second blocks
with a one-day difficulty-adjustment period. Its automatic scheduling has not
been adapted to those parameters, so it should not be relied on to produce the
network's target cadence or converge to the requested difficulty. See
[`Generate.INTERVAL`](miner) and the Signet parameters in
[`src/kernel/chainparams.cpp`](../../src/kernel/chainparams.cpp).

Other options
-------------

The --debug and --quiet options are available to control how noisy the signet miner's output is. Note that the --debug, --quiet and --cli parameters must all appear before the subcommand (`generate` or `calibrate`) if used.

Instead of specifying --ongoing, you can specify --max-blocks=N to mine N blocks and stop.

The --set-block-time option is available to manually move timestamps forward or backward (subject to the rules that blocktime must be greater than mediantime, and dates can't be more than two hours in the future). It can only be used when mining a single block (ie, not when using --ongoing or --max-blocks greater than 1).

Instead of using a single address, a ranged descriptor may be provided via the --descriptor parameter, with the reward for the block at height H being sent to the H'th address generated from the descriptor.

The --min-nbits option still sets the scheduling target to the inherited value
`1e0377ae`; this is not ConnectCoin Signet's minimum difficulty. The current
network proof-of-work limit corresponds to compact target `1f00ffff`. Use an
explicit --nbits scheduling target and account for the scheduling limitation
above; neither option overrides the difficulty required by the block template.

By default, the signet miner schedules blocks without random interval variation. The --poisson option adds simulated Poisson variation, subject to the same scheduling limitation.

Using the --multiminer parameter allows mining to be distributed amongst multiple miners. For example, if you have 3 miners and want to share blocks between them, specify --multiminer=1/3 on one, --multiminer=2/3 on another, and --multiminer=3/3 on the last one. If you want one to do 10% of blocks and two others to do 45% each, --multiminer=1-10/100 on the first, and --multiminer=11-55 and --multiminer=56-100 on the others. Note that which miner mines which block is determined by the previous block hash, so occasional runs of one miner doing many blocks in a row is to be expected.

When --multiminer is used, if a miner is down and does not mine a block within five minutes of when it is due, the other miners will automatically act as redundant backups ensuring the chain does not halt. The --backup-delay parameter can be used to change how long a given miner waits, allowing one to be the primary backup (after five minutes) and another to be the secondary backup (after six minutes, eg).

The --standby-delay parameter can be used to make a backup miner that only mines if a block doesn't arrive on time. This can be combined with --multiminer if desired. Setting --standby-delay also prevents the first block from being mined immediately.

Advanced usage
--------------

The `generate` subcommand requests a block template with the `signet`, `segwit`,
and `typedoutputs` rules, constructs the block and its typed coinbase output,
checks that the Signet challenge is trivial, grinds RandomX proof of work using
the template's `randomxkey`, and submits the block. A `--grind-cmd` is required.

There is no PSBT block-signing pipeline: `genpsbt` and `solvepsbt` are not
available subcommands, and the current miner does not support external or
hardware-wallet signing of Signet challenges.
