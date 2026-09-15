# Linearize
Construct a linear, no-fork, best version of the ConnectCoin blockchain.

## Step 1: Download hash list

Run the scripts from this directory. Create `linearize.cfg` using
[`example-linearize.cfg`](example-linearize.cfg) as a template, and configure
the selected network, credentials, input paths, and new output paths before
running either script.

    $ ./linearize-hashes.py linearize.cfg > hashlist.txt

Required configuration file settings for linearize-hashes:
* RPC: `datadir` (Required if `rpcuser` and `rpcpassword` are not specified).
  This must be the directory containing `.cookie`, such as
  `~/.connectcoin/testnet4`, not the base data directory passed to `connectcoind`.
* RPC: `rpcuser`, `rpcpassword` (Required if `datadir` is not specified)

Optional config file setting for linearize-hashes:
* RPC: `host`  (Default: `127.0.0.1`)
* RPC: `port` (Default: `48178`, the current Testnet4 beta RPC port; set it
  explicitly for other networks.)
* Blockchain: `min_height`, `max_height` (inclusive; defaults: `0`, `313000`).
  Set `max_height` to a height available on the selected node; the script does
  not automatically discover the tip. Use `min_height=0` for the two-step
  export below, which requires the genesis block in the hash list.
* `rev_hash_bytes`: If true, the written block hash list will be
byte-reversed. (In other words, the hash returned by getblockhash will have its
bytes reversed.) False by default. Intended for generation of
standalone hash lists but safe to use with linearize-data.py, which will output
the same data no matter which byte format is chosen.

The `linearize-hashes` script requires a connection, local or remote, to a
JSON-RPC server. Running `connectcoind` or `connectcoin-qt -server` will be sufficient.

## Step 2: Copy local block data

    $ ./linearize-data.py linearize.cfg

Required configuration file settings:
* `output_file`: The file that will contain the final blockchain.
      or
* `output`: Output directory for linearized `blocks/blkNNNNN.dat` output.
* `genesis`: The selected chain's genesis hash, as returned by
  `connectcoin-cli getblockhash 0` (64 hexadecimal characters).
* `netmagic`: The selected chain's network magic (8 hexadecimal characters),
  available as `net.magic` from `connectcoin-util getchainparams`.

Use an unused `output_file`, or a new empty `output` directory separate from
the input blocks directory. Output files are opened for writing and existing
files with the same names are overwritten. If both output settings are present,
`output` takes precedence.

There are no implicit genesis or network-magic defaults. Test networks can be
reset, and old block files must not be mixed with a new chain. The example
configuration uses the P2C mask v1 Testnet4 network; for another network, select that
network explicitly in both commands and update the port and paths as well.

Optional config file setting for linearize-data:
* `debug_output`: Some printouts may not always be desired. If true, such output
will be printed.
* `file_timestamp`: Set each file's last-accessed and last-modified times,
respectively, to the current time and to the timestamp of the most recent block
written to the script's blockchain.
* `input`: connectcoind blocks/ directory containing blkNNNNN.dat
* `hashlist`: text file containing list of block hashes created by
linearize-hashes.py.
* `max_out_sz`: Size limit for numbered block files created by the `output`
directory option. It does not limit a single `output_file`.
(Default: `1000*1000*1000 bytes`)
* `out_of_order_cache_sz`: If out-of-order blocks are being read, the block can
be written to a cache so that the blockchain doesn't have to be sought again.
This option specifies the cache size. (Default: `100*1000*1000 bytes`)
* `rev_hash_bytes`: If true, the block hash list written by linearize-hashes.py
will be byte-reversed when read by linearize-data.py. See the linearize-hashes
entry for more information.
* `split_timestamp`: Split blockchain files when a new month is first seen, in
addition to reaching a maximum file size (`max_out_sz`).
Use this only with `output`. Keep `split_timestamp=0` when using `output_file`:
otherwise each monthly split reopens the same filename in write mode and
overwrites the earlier exported blocks.
