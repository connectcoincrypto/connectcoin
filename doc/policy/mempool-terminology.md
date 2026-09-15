# Fee and Size Terminology in Mempool Policy

## Transaction sizes

A transaction's **weight** follows BIP 141: three times its serialized size
without witness data, plus its serialized size including witness data. Its
**BIP 141 virtual size** is `ceil(weight / 4)`, in virtual bytes (vB).

Mempool policy also accounts for signature-operation cost. Let `s` be the
transaction's sigop cost and `b` the node's configurable `-bytespersigop` value:

- **Sigops-adjusted weight:** `max(weight, s * b)`.
- **Sigops-adjusted virtual size:** `ceil(max(weight, s * b) / 4)`.

The adjusted size helps account for both weight and signature-operation limits
when selecting transactions. The mempool keeps the adjusted weight internally
to avoid losing precision through virtual-size rounding.

## RPC fields

The meaning of `vsize` depends on the RPC; it is not universally sigops-adjusted.

| Field and context | Meaning |
| --- | --- |
| `weight` in decoded transactions and mempool entries | BIP 141 weight |
| `vsize` in decoded transactions, such as `decoderawtransaction` and verbose `getrawtransaction` | BIP 141 virtual size |
| `vsize_bip141` in mempool entries, `testmempoolaccept`, and `submitpackage` results | BIP 141 virtual size |
| `vsize_adjusted` in those same results | Sigops-adjusted virtual size; their `vsize` field is a deprecated alias for this value |

Mempool entry fields `ancestorsize` and `descendantsize` sum the sigops-adjusted
virtual sizes in the associated set, including the transaction itself.
`chunkweight` and `clusterweight` sum sigops-adjusted weights in the chunk or
cluster. Consult each RPC's help for its exact result schema.

## Fees

A transaction's **base fee** is the difference between its input and output
values. Its **modified fee** adds any **fee delta** introduced by the
`prioritisetransaction` RPC. This local adjustment is used for mempool policy
and block-template selection; it does not change the fee actually paid by the
transaction.
