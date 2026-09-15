Unauthenticated REST Interface
==============================

The REST API can be enabled with the `-rest` option.

The interface runs on the same port as the JSON-RPC interface: 48178 for the
default testnet4 beta, 48175 for testnet3, 48181 for signet, and 48184 for
regtest. Mainnet is unavailable; its reserved RPC port is 48172.

REST Interface consistency guarantees
-------------------------------------

The [same guarantees as for the RPC Interface](/doc/JSON-RPC-interface.md#rpc-consistency-guarantees)
apply.

Default HTTP caching
--------------------

REST responses include `Cache-Control` headers by default:

* `public, immutable, max-age=86400` for `/block` and `/block/notxdetails`
  binary and hex responses, `/blockpart`, `/blockfilter` and `/spenttxouts` in
  all formats, and `/deploymentinfo/<BLOCKHASH>.json`. The TTL is deliberately
  short so caches do not hold older response shapes across software upgrades.
* `no-store` for `/block` and `/block/notxdetails` JSON, `/tx`, `/headers`,
  `/blockfilterheaders`, `/blockhashbyheight`, `/chaininfo`, `/mempool`,
  `/getutxos`, `/deploymentinfo.json`, and all error responses. These responses
  can change with active chain or node state and do not currently provide cache
  validators such as `ETag` or `Last-Modified`.

If you front `connectcoind` with a reverse proxy or CDN such as Caddy or nginx with
the headers-more module, you can override these defaults there. Keep overrides
scoped to responses you know are safe to cache more aggressively.

Supported API
-------------

#### Transactions
`GET /rest/tx/<TX-HASH>.<bin|hex|json>`

Given a transaction hash: returns a transaction in binary, hex-encoded binary, or JSON formats.
Responds with 404 if the transaction doesn't exist.

By default, this endpoint will only search the mempool.
To query for a confirmed transaction, enable the transaction index via "txindex=1" command line / configuration option.

#### Blocks
- `GET /rest/block/<BLOCK-HASH>.<bin|hex|json>`
- `GET /rest/block/notxdetails/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns a block, in binary, hex-encoded binary or JSON formats.
Responds with 404 if the block doesn't exist.

The HTTP request and response are both handled entirely in-memory.

With the /notxdetails/ option JSON response will only contain the transaction hash instead of the complete transaction details. The option only affects the JSON response.

- `GET /rest/blockpart/<BLOCK-HASH>.<bin|hex>?offset=<OFFSET>&size=<SIZE>`

Given a block hash: returns a block part, in binary or hex-encoded binary formats.
Responds with 404 if the block is absent or its data is unavailable, and 400
if `offset` or `size` is missing or the requested byte range is invalid.

#### Blockheaders
`GET /rest/headers/<BLOCK-HASH>.<bin|hex|json>?count=<COUNT=5>`

Given a block hash: returns <COUNT> amount of blockheaders in upward direction.
Returns empty if the block doesn't exist or it isn't in the active chain.

*Deprecated (but not removed) since 24.0:*
`GET /rest/headers/<COUNT>/<BLOCK-HASH>.<bin|hex|json>`

#### Blockfilter Headers
`GET /rest/blockfilterheaders/<FILTERTYPE>/<BLOCK-HASH>.<bin|hex|json>?count=<COUNT=5>`

Given a block hash: returns <COUNT> amount of blockfilter headers in upward
direction for the filter type <FILTERTYPE>.
Returns empty if the block doesn't exist or it isn't in the active chain.

*Deprecated (but not removed) since 24.0:*
`GET /rest/blockfilterheaders/<FILTERTYPE>/<COUNT>/<BLOCK-HASH>.<bin|hex|json>`

#### Blockfilters
`GET /rest/blockfilter/<FILTERTYPE>/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns the block filter of the given block of type
<FILTERTYPE>.
Responds with 404 if the block doesn't exist.

#### Blockhash by height
`GET /rest/blockhashbyheight/<HEIGHT>.<bin|hex|json>`

Given a height: returns hash of block in best-block-chain at height provided.
Responds with 404 if block not found.

#### Spent transaction outputs
`GET /rest/spenttxouts/<BLOCK-HASH>.<bin|hex|json>`

Given a block hash: returns a collection of spent transaction output lists,
one per transaction in the block.
Responds with 404 if the block doesn't exist or its undo data is not available.

#### Chaininfos
`GET /rest/chaininfo.json`

Returns various state info regarding block chain processing.
Only supports JSON as output format.
Refer to the `getblockchaininfo` RPC help for details.

#### Deployment info
`GET /rest/deploymentinfo.json`
`GET /rest/deploymentinfo/<BLOCKHASH>.json`

Returns an object containing various state info regarding deployments of
consensus changes at the current chain tip, or at <BLOCKHASH> if provided.
Only supports JSON as output format.
Refer to the `getdeploymentinfo` RPC help for details.

#### Query UTXO set
- `GET /rest/getutxos/<TXID>-<N>/<TXID>-<N>/.../<TXID>-<N>.<bin|hex|json>`
- `GET /rest/getutxos/checkmempool/<TXID>-<N>/<TXID>-<N>/.../<TXID>-<N>.<bin|hex|json>`

The getutxos endpoint allows querying the UTXO set, given a set of outpoints.
With the `/checkmempool/` option, the mempool is also taken into account.
The request and response framing is inherited from
[BIP64](https://github.com/bitcoin/bips/blob/master/bip-0064.mediawiki), but each
returned output in `bin` and `hex` responses uses ConnectCoin's
[typed-output serialization](typed-outputs.md#consensus-wire-format).
It does not use Bitcoin's value-and-Script output encoding. In JSON responses,
`scriptPubKey` describes the output's
[in-memory compatibility view](typed-outputs.md#internal-compatibility-view).

Request template for an outpoint on testnet4; replace `<TXID>` and `<N>` with
the transaction ID and output index:

```sh
curl "http://127.0.0.1:48178/rest/getutxos/checkmempool/<TXID>-<N>.json"
```

Illustrative response template for one available type-1 output. Angle-bracket
values are placeholders, not network data; the unquoted placeholders represent
JSON numbers:

```
{
   "chainHeight" : <tip-height>,
   "chaintipHash" : "<tip-hash>",
   "bitmap": "1",
   "utxos" : [
      {
         "height" : <output-height>,
         "value" : <amount-in-CC>,
         "scriptPubKey" : {
            "asm" : "1 <32-byte-x-only-public-key-hex>",
            "desc" : "rawtr(<32-byte-x-only-public-key-hex>)#<checksum>",
            "hex" : "5120<32-byte-x-only-public-key-hex>",
            "type" : "witness_v1_taproot",
            "address" : "<testnet4-Bech32m-address>"
         }
      }
   ]
}
```

#### Memory pool
`GET /rest/mempool/info.json`

Returns various information about the transaction mempool.
Only supports JSON as output format.
Refer to the `getmempoolinfo` RPC help for details.

`GET /rest/mempool/contents.json?verbose=<true|false>&mempool_sequence=<false|true>`

Returns the transactions in the mempool.
Only supports JSON as output format.
Refer to the `getrawmempool` RPC help for details. Defaults to setting
`verbose=true` and `mempool_sequence=false`.

*Query parameters for `verbose` and `mempool_sequence` available in 25.0 and up.*


Risks
-------------
Running a web browser on the same node with a REST enabled connectcoind can be a risk. Accessing prepared XSS websites could read out tx/block data of your node by placing links like `<script src="http://127.0.0.1:48178/rest/tx/1234567890.json">` which might break the nodes privacy.
