# Mempool Replacements

## Current Replace-by-Fee Policy

ConnectCoin uses full replace-by-fee: a transaction does not need to signal
replaceability for these rules to apply.

A transaction conflicts with an in-mempool transaction ("directly conflicting transaction") if they
spend one or more of the same inputs. A transaction may conflict with multiple in-mempool
transactions.

Fee comparisons below use modified fees: base fees plus any local fee delta from
`prioritisetransaction`. Such deltas affect replacement policy and the feerate
diagram, but do not change the fees actually paid by the transactions.

A transaction ("replacement transaction") may replace its directly conflicting transactions and
their in-mempool descendants (together, "original transactions") if, in addition to passing all
other consensus and policy rules, each of the following conditions are met:

1. The replacement transaction has an absolute modified fee of at least the sum of the
   original transactions' modified fees.

   *Rationale*: Only requiring the replacement transaction to have a higher feerate could allow an
   attacker to bypass node minimum relay feerate requirements and cause the network to repeatedly
   relay slightly smaller replacement transactions without adding any more fees. Additionally, if
   any of the original transactions would be included in the next block assembled by an economically
   rational miner, a replacement policy allowing the replacement transaction to decrease the absolute
   fees in the next block would be incentive-incompatible.

2. The additional modified fees (the replacement's modified fee minus the sum of the original
   transactions' modified fees) pay for the replacement transaction's bandwidth at or
   above the rate set by the node's incremental relay feerate. For example, if the incremental relay
   feerate is 0.1 connect/vB and the replacement transaction is 500 virtual bytes total, then the
   replacement needs a modified fee at least 50 connects higher than the sum of the originals' modified fees.

   *Rationale*: Try to prevent DoS attacks where an attacker causes the network to repeatedly relay
   transactions each paying a tiny additional amount in fees, e.g. just 1 connect.

3. The number of distinct clusters corresponding to conflicting transactions does not exceed 100.

   *Rationale*: Limit CPU usage required to update the mempool for so many transactions being
   removed at once.

4. The feerate diagram of the mempool must be strictly improved by the replacement transaction.

   *Rationale*: Cumulative modified fees must not fall at any size in the diagram,
   and must improve somewhere. This is not a guarantee that the actual fees paid
   in every future block will increase.


The incremental relay feerate used to calculate the additional fee is distinct
from `-minrelaytxfee` and configurable using `-incrementalrelayfee`. Its default
is 0.1 connect/vB; `getmempoolinfo` reports the active value in CC/kvB.

For P2C claims, changing the transaction ID also changes the TLS claim challenge.
A fee replacement that changes that ID therefore needs a new connection proof;
it cannot reuse the original witness. See [Pay-to-connect](../pay-to-connect.md#claim-challenge).
