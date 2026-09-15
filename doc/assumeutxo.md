# Assumeutxo Usage

Assumeutxo can bootstrap a validating `connectcoind` instance from a UTXO
snapshot whose commitment is included in the selected chain's parameters.
The public testnet4 beta currently has no supported snapshot commitments, so
`loadtxoutset` cannot bootstrap it. Testnet3 and signet also have none; mainnet
is unavailable. Regtest includes commitments for deterministic test fixtures,
which do not authorize arbitrary regtest snapshots.

For notes on the design of Assumeutxo, please refer to [the design doc](/doc/design/assumeutxo.md).

## Loading a snapshot

Loading requires a snapshot matching a supported block and UTXO-set hash in the
selected chain's parameters. A downloaded or locally generated snapshot does
not establish that commitment. There is currently no canonical source for
ConnectCoin snapshots.

For a network and snapshot with a supported commitment, use the RPC command
`loadtxoutset` to load it. This command is not currently usable for public beta
bootstrapping:

```
$ connectcoin-cli -rpcclienttimeout=0 loadtxoutset /path/to/input
```

After the snapshot has loaded, the syncing process of both the snapshot chain
and the background IBD chain can be monitored with the `getchainstates` RPC.

### Pruning

A pruned node can load a supported snapshot. To save space, it's possible to
delete the snapshot file after `loadtxoutset` succeeds.

The minimum `-prune` setting is 550 MiB, but this functionality ignores that
minimum and uses at least 1100 MiB.

As the background sync continues there will temporarily be two chainstate
directories. Their size depends on the chain's UTXO set and can exceed the
downloaded snapshot's size.

### Indexes

Indexes work but don't take advantage of this feature. They always start building
from the genesis block and can only apply blocks in order. Once the background
validation reaches the snapshot block, indexes will continue to build all the
way to the tip.


For indexes that support pruning, note that these indexes only allow blocks that
were already indexed to be pruned. Blocks that are not indexed yet will also
not be pruned.

This means that, if the snapshot is old, then a lot of blocks after the snapshot
block will need to be downloaded, and these blocks can't be pruned until they
are indexed, so they could consume a lot of disk space until indexing catches up
to the snapshot block.

## Generating a snapshot

The RPC command `dumptxoutset` can export a snapshot of the current tip using
type `latest`, including on testnet4. Exporting a snapshot does not make it
loadable with `loadtxoutset`: loading still requires the matching commitment
in the receiving node's chain parameters.

To export an earlier state, specify the named `rollback` option with the desired
height or block hash; the required block and undo data must be available.
Using type `rollback` without an explicit height or hash selects the latest
supported snapshot height and therefore requires an existing commitment. It
does not work on the public beta networks. On a chain with a supported snapshot,
regenerating it allows comparison with the committed UTXO-set hash.

Example export of the current state:

```
$ connectcoin-cli -rpcclienttimeout=0 dumptxoutset /path/to/output latest
```

Rollback copies the UTXO set into a temporary database and disconnects blocks
from that copy. The active chain remains intact and peer connections are not
disabled by the export. Allow enough disk space for the temporary database and
output, or enough RAM if using `in_memory=true`. An unclean shutdown may leave
a temporary database requiring manual cleanup; inspect it before removing it.

`dumptxoutset` takes some time to complete, independent of hardware and
what parameter is chosen. Because of that it is recommended to increase the RPC
client timeout value (use `-rpcclienttimeout=0` for no timeout).
