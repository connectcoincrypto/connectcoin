# P2C in the wallet

The **P2C** tab (Alt+5) has separate **Create bounties** and **Automatic claims**
pages. Creating bounties uses the same type-2 outputs as `sendtop2c`. During
confirmation the GUI makes a bounded TLS capability probe, without sending an
HTTP request. Automatic claiming is disabled until explicitly enabled.

## Creating bounties

1. Select the funding wallet and open **P2C**.
2. Enter a lower-case ASCII domain, without a scheme, path, port, or trailing dot.
   International domains must already be converted to punycode.
3. Set the reward **per output** and the number of independent outputs (1–1000).
4. Select either leading zero bits (0–256) or an exact 64-character hexadecimal
   maximum hash. The roots selector lists supported immutable certificate bundles.
5. Use automatic fee selection or specify a custom rate **per 1,000 virtual bytes**.
   The displayed unit selector applies to that rate. Wallet fee safeguards remain active.
   Extreme rates that cannot be evaluated safely are rejected before transaction construction.
6. Click **Review P2C** and unlock the wallet if requested. Review the domain,
   difficulty, roots version, total rewards, transaction count, fees, and total debit.
   During the existing three-second confirmation delay, the GUI offers only
   `rsa_pss_rsae_sha256` and `rsa_pss_pss_sha256`. If a complete, authenticated
   handshake succeeds before the deadline, it selects mask `6` (both RSA schemes).
   Failure, timeout, cancellation of the probe, or a busy probe worker keeps mask
   `7` (all three supported schemes). The chosen mask is displayed and frozen
   before **Send P2C** is enabled. A successful probe confirms the selected
   server's current capability, not future availability or all DNS endpoints.
7. Confirm **Send P2C**, or cancel without sending. Submission IDs appear on the page.

In transaction history, each P2C output shows `P2C: domain` in the Label column.
The domain comes directly from that output, including for previously created or
RPC-created transactions. It is searchable and included when copying labels or
exporting the history; it is not an editable address-book entry.

Large requests are split into standard-sized transactions with independent confirmed
inputs. Enough total balance alone may not be sufficient: a split also needs enough
separate confirmed inputs. Inputs are temporarily reserved while awaiting approval;
cancellation releases those reservations without unlocking unrelated user-locked coins.
The wallet signs both mask alternatives before relocking, using identical inputs,
amounts, fees and transaction weights. Selecting the mask requires no second unlock.
Once sending is enabled, the selected signed batch is immutable and is submitted
without regenerating fees. Closing the confirmation releases its reservations.
The capability probe never bypasses configured proxies or connects to private
addresses, and at most four probes can remain pending across GUI pages.

Bounties can be claimed by anyone presenting the required connection proof. The
funding wallet cannot refund them with an ordinary signature. Submission is not
confirmation, and the wallet's normal broadcast configuration still applies. A batch
is not atomic across transactions: a storage failure may leave some committed. If
submission reports an error, check the transaction IDs and wallet history before retrying.

This initial interface requires a wallet with local private keys. Watch-only and
external-signer workflows remain available through the existing raw transaction/PSBT
tools, not through this page. No consensus rules or trust roots are changed by this UI.

## Manual claim backend

The wallet exposes `preparep2cclaim` and `submitp2cclaim` for external generators,
including the independent p2c-tools. Neither of these two RPCs starts a connection,
scans for bounties, or runs a background job.

```
connectcoin-cli -rpcwallet=claimant preparep2cclaim "funding_txid" 0
connectcoin-cli -rpcwallet=claimant submitp2cclaim "prepared_hex" "proof_hex"
```

Preparation requires one confirmed type-2 output, possibly funded by somebody
else. By default it reserves a new P2PK receiving destination in this wallet and deducts
fees exclusively from the bounty: no other wallet coins are spent. Local-key
wallets may remain locked if their keypool has an available receiving address.
Alternatively, pass the optional `address` argument to pay an explicit type-1
P2PK address on this network without reserving a wallet key. This also works
with a watch-only/external-signer wallet: claiming needs no private-key signature.
When submitting a proposal with an external payout, explicitly authorize the
same address with `submitp2cclaim "prepared_hex" "proof_hex" "REWARD_ADDRESS"`.
Without that argument the payout must still belong to this wallet; a different
explicit address is rejected. Empty addresses select the wallet default.

The preparation result supplies the canonical on-chain domain, work target,
root version, signature algorithms mask, tip median time, fixed transaction hex and exact ClientHello
challenge. A proof generator must use these values unchanged. `proof_size`
defaults to the full 65536-byte consensus limit and determines the fee budget,
including transaction/witness framing. An explicit smaller budget can reduce
fees, but a larger actual proof may fail the current mempool minimum. Unused
budget remains a fee. Fees cannot be changed after generating the proof
without generating a new challenge and doing the TLS work again. The optional
`fee_rate` is in connects/vB, consistently with other wallet RPCs.

Submission checks that the payout belongs to this wallet or matches the explicitly
authorized address, the input is a
confirmed available bounty, and the proof meets the challenge and work target.
It then asks the node to perform full current mempool/consensus validation,
including the certificate chain, CertificateVerify and tip median time, before
storing anything. A competing claim in the mempool or a reorg can invalidate a
proposal. The final submission is rechecked by the node; no UTXO reservation
can prevent another network participant from claiming first.

`stored` means wallet storage succeeded. `in_mempool` reports a subsequent
local observation, not confirmation: `walletbroadcast=0`, a concurrent block,
or a competing spend can affect it. After a storage/submission error inspect
wallet history before retrying. These RPCs do not automatically retry, bump
fees, or submit proof data obtained from an untrusted JSON envelope.

## Native automatic claims

Open **P2C → Automatic claims**, select a connection rate, simultaneous connections,
and optionally a comma-separated domain allowlist. Leave **Reward address**
empty to pay this wallet (the default), or enter a type-1 P2PK address for this
network. Confirm **Apply / start** and check the reward target in the status.
Successful proofs are submitted automatically, without a separate redeem click.
The wallet needs local keys for the default target, but not for an explicit address.
Both modes require `walletbroadcast=1`, but no pre-existing balance.

The equivalent RPCs are:

```
connectcoin-cli -rpcwallet=claimant setp2cclaiming 1 4 '["example.com"]'
connectcoin-cli -rpcwallet=claimant getp2cclaimstatus
connectcoin-cli -rpcwallet=claimant setp2cclaiming 0
```

The examples explicitly select four simultaneous connections. The GUI, initial
worker status, and RPC default use **1000** simultaneous connections; omitting
the RPC's second argument (or passing `null`) selects that default. HTTPS remains
disabled when the wallet loads until you explicitly enable it.

The optional fourth argument of `setp2cclaiming` is `address`. For example,
`setp2cclaiming 1 4 '["example.com"]' "REWARD_ADDRESS"` sends rewards there.
`getp2cclaimstatus.reward_address` reports this setting; an empty string means
the wallet default. Omitting it on reconfiguration restores the default.

Changing the target stops and joins old searches before starting new ones.
Unfinished proposals with a different payout are replaced because the payout
is bound into their TLS challenge. **Completed proofs retain their original
authorized destination**, saved with the proof even across wallet reloads;
changing the setting never redirects or discards completed work. The new target
applies to new searches. No destination setting is restored automatically on
wallet load, and HTTPS still starts disabled.

Automatic claims use the greatest of the wallet/relay minimum, mining minimum and
current mempool fee floor, budgeting for the full maximum proof. This works without fee-estimation
history or enabling fallback fees; normal maximum-fee and dust safeguards still
apply. Unused proof budget stays a fee, and the chosen rate does not guarantee
prompt confirmation if demand rises before submission.

- `0` stops the worker and its HTTPS activity. It is **not** unlimited.
- `-1` selects unlimited rate explicitly; Qt exposes a separate checkbox and
  displays `Unlimited` in the status, not the RPC sentinel `-1`.
- Positive rates limit connection starts across all workers **in this wallet**;
  they are not a node-wide limit shared by multiple wallets. Concurrency accepts
  any positive 32-bit integer (default **1000**), with no separate 64-connection cap. It is a maximum,
  not guaranteed throughput: OS thread/socket limits, memory, the selected rate
  and server responsiveness determine actual parallelism. If the OS refuses
  additional connection threads, already-started workers keep searching and
  `last_error` reports the reduced capacity alongside any connection error.
  Growth is not retried until claiming is reconfigured. If even the first worker
  cannot start, the search stops with an error. Stop/unload still joins every worker.
- An empty allowlist permits all supported public domains with confirmed bounties.
  HTTPS reveals your IP to those domains. Connections are direct, on port 443;
  private/unroutable addresses are excluded. A configured name, IPv4 or IPv6 proxy
  disables this path rather than being silently bypassed. SOCKS support is not
  implemented for the claim generator.

The node maintains a **shared in-memory catalog of confirmed P2C bounties**.
The first lookup scans a flushed UTXO snapshot outside the chain lock. Subsequent
lookups apply only new blocks (their outputs and spent inputs), plus block undo
during reorganizations. If the required history was pruned, the catalog is rebuilt
from the current UTXO snapshot. It is not persisted: restarting the node requires
one initial scan again. Wallets sharing the node's chain interface reuse the
catalog, rather than each flushing/scanning the entire UTXO database.

Automatic discovery requests only bounties created in the **last 600 blocks**,
including the tip (about 100 minutes at the 10-second target spacing). Creation
heights are retained by the catalog, including after a rebuild or reorg. A
height index selects the recent range without walking ancient entries or
passing them to wallet eligibility checks; this does not add per-bounty UTXO
database lookups. The
initial catalog still scans the UTXO snapshot once. This is a periodic search
preference, not an expiry rule: handshakes already in flight may finish after
a bounty ages out, and completed proofs are still submitted. Older bounties
remain valid and manually claimable.

Each wallet refreshes eligible bounties and expected-return priorities every
**five seconds**, visiting this P2C-only catalog. Competing mempool spends and
wallet coin locks temporarily exclude bounties; they can become eligible again
after eviction/unlocking. Availability checks of cached/in-flight work are batched
at refresh time, not performed before every repeated TLS attempt. A competitor
can therefore cause some wasted work until the next refresh, but submission
always rechecks current availability and every consensus/policy rule.

**A domain is selected for each new connection**, not for a 30-second batch.
Persistent connection workers share the scheduler. Guaranteed round-robin
assignments alternate with extra assignments for the domain with the highest
performance-weighted expected net return. Economic preference does not move the
fair cursor: with A,
B and C eligible and A as the economic winner, assignments are A, A, B, A, C, A,
then repeat. Concurrent connections may start/finish in a different order.
This balances economic preference with exploration; it is not proportional
allocation. The economic winner uses its best eligible bounty, not the sum or
count of its outputs. Unusable bounties do not earn extra economic assignments.

Each `(domain, signature_algorithms_mask)` pair keeps a rolling
`deque<pair<bool, double>>` of the **last 100 completed TCP/TLS attempts**,
shared across its bounties and resolved IPs with the same mask. An unsupported
RSA-only bounty therefore cannot poison the measured success rate of an
ECDSA-capable bounty on that domain. Rotation and DNS are still grouped by
domain, not by mask. The
boolean records capture through **CertificateVerify**; seconds measure elapsed
TCP/TLS effort with a monotonic clock. TCP failures, TLS errors and timeouts count
as failures. A completed capture counts as a success regardless of whether its
hash meets the bounty target; certificate/claim acceptance is still verified
separately. DNS resolution, rate-limit waits, scheduler waits, retry backoff and
subsequent certificate verification are not part of this duration. Locally
cancelled incomplete attempts (stop, spent bounty or duplicate proof) are excluded,
since they do not establish whether the server could complete the handshake.

At each five-second refresh the multiplier for each domain/mask pair is:
`(0.1 + successful_captures) / (0.02 + total_attempt_seconds)`.
An untried pair therefore starts at **5 captures/second of effort**. The score
for extra assignments is the highest eligible bounty score after multiplying
each mask's expected net return by its own rate. This favors reliable, fast connections while retaining guaranteed
round-robin exploration. Timing scores use floating point; exact economic keys
break rounded ties. Durations of simultaneous attempts are summed individually,
not measured as a shared wall-clock interval, so raising concurrency alone does
not multiply the observed efficiency.

Automatic search ignores each bounty whose expected net return is **less than
1000 connects per second of connection effort** (1000 exactly is eligible):
`(reward - claim_fee) * (target + 1) / 2^256 * measured_capture_rate`.
As a fast rejection, a target whose most significant 64 bits are all zero is
ignored immediately: even MAX_MONEY at the maximum smoothed rate of 5005/s
cannot reach this floor. This includes targets requiring 256 zero bits.
The threshold uses the domain/mask pair's last-100 history or the initial 5/s prior, not
the user-configured connection limit or aggregate concurrency. The measured
score uses the same floating-point precision as domain ranking.

The filter applies before DNS, receiving-key reservation and TLS, including
guaranteed rotation turns and saved unfinished proposals. Eligibility is
recalculated every five seconds as fees, bounties and measured efficiency
change. In-flight handshakes may finish; completed proofs are still retained
and submitted. A domain with only below-floor bounties is not probed just to
refresh its speed estimate. This is wallet search policy, not a consensus
restriction or a restriction on preparing/submitting a claim manually.

Each bounty also has a local connection-start budget. With
`p = (target + 1) / 2^256`, new attempts stop once its count is **strictly greater
than `2 / p`**. For example, `p = 1/2` permits five starts: four equals `2 / p`,
and the fifth exceeds it. The comparison uses exact 320-bit arithmetic. Starts
are serialized across workers, so concurrency cannot overshoot the budget;
already started handshakes may finish and successful proofs are not discarded.
TCP/TLS failures consume this budget too; DNS resolution and waiting do not.
Counters are per outpoint, not per domain or payout address, and survive
five-second refreshes, proposal-cache eviction and stop/start while the wallet
is loaded. They are **only in memory** and reset on wallet unload/restart.
Counters for confirmed-spent or aged-out bounties are also collected after
in-flight work/proofs drain, bounding retained history to recent work. Changing
the domain allowlist, temporary mempool spends or locking a coin does not reset
a recent bounty's budget. A reorg restoring an already forgotten bounty can
start a fresh budget; this is deliberately a soft local search policy.
For independent valid trials with small `p`, failing around `2 / p` times has
probability approximately 13.5%; this is a resource policy, not evidence that
a domain is malicious, especially when some attempts fail before hashing.

The oldest attempt is removed when the window exceeds 100. Histories survive
priority refreshes, temporary bounty ineligibility and stop/start in the same
loaded wallet. They are in-memory only: unloading the wallet resets them. A
domain/mask pair with no remaining matching confirmed bounties is forgotten once its
in-flight work drains. These measurements affect local scheduling only, never
consensus, the global connection limit or within-domain bounty ordering.

There is **no per-domain connection quota or configurable per-domain limit**.
Only the wallet-wide rate and concurrency settings limit traffic. A sole eligible
domain may use all that capacity. Low-rate workers do not preassign a long queue
of future connections: assignments are made when a connection slot is due.
`domain_rounds` is retained for RPC compatibility, but now counts connection
assignments since configuration, not timed rounds. `schedule_refreshes` counts
completed catalog/priority refreshes. The displayed domain is the last assigned
domain; other domains can be in flight simultaneously.

Within a domain, bounties rotate in descending expected net return:
`(target + 1) / 2^256 * (reward - claim_fee)`. The inclusive `+1` matches consensus's
`hash <= target`. Exact 320-bit integer numerators avoid floating-point rounding
and overflow at the maximum target. Existing proposals retain their actual
payout/fee; new proposals use the full-proof fee calculation. Equal priorities
use outpoint order. A cursor preserves progress through large groups across
refreshes, including bounties beyond the 256-entry fixed-proposal cache.
The cache never evicts an in-flight challenge or a completed proof. This is a
memory bound on distinct proposals, not a cap on connections or visited domains.

Priority refreshes **do not cancel or restart healthy in-flight TLS handshakes**.
Certificate validation follows the refreshed chain median time even in searches
lasting hours; updating that time never changes the transaction or challenge.
A successful proof cancels only duplicate attempts for that bounty, is saved
durably, and is automatically submitted by the coordinator. A spent/locked/reorged
bounty cancels only its affected work at the availability refresh. Every attempt
uses its own bounty-specific challenge; proofs are never reused for other outputs.

DNS results are reused for 60 seconds, with resolved addresses rotated between
attempts (including at concurrency 1). Resolution is outside the scheduler lock:
one slow lookup does not block other connection workers on other domains.
Failed DNS backs off for two seconds for that domain. Failed TLS or rejected
certificates back off for one second per connection worker. These are retry
backoffs, not per-domain traffic quotas. All waits are interruptible on stop,
but an OS DNS call may delay the worker using that call until it returns.

If no matching bounties exist, the state is `waiting for bounties`; if matching
bounties exist but none are eligible (for example, locked or uneconomic), it is
`waiting for eligible bounties`. Both recheck after five seconds. No connection
threads are created speculatively when there is no eligible work.
TCP setup is bounded to one second and TLS capture to ten seconds, independent
of the ranking refresh. HTTPS is independent of the P2P network toggle; use
**Stop HTTPS (0)** to stop it.

This protects against monopolization by one domain, not an attacker controlling
many domains. It cannot force a server to supply winning transcripts. Claim
generation and its cached discovery metadata do not change consensus rules.

Fixed unsigned proposals and completed proofs are stored in the wallet database.
A completed proof is saved **before** submission. Concurrent successes are queued
durably (one per proposal), not discarded when another bounty succeeds. The saved
state still reads older wallets' single-proof slot. Reloading/unloading the wallet
stops the worker, and loading always starts with HTTPS disabled; explicitly enable
it to resume. A completed proof rejected while its bounty remains available is
retained, with a diagnostic, and the worker stops. Re-enabling retries acceptance
without regenerating TLS work. A proof invalidated permanently by chain time or
policy requires operator attention; there is no automatic fee bump that changes
the bound transaction. The bounded proposal cache may rotate unsigned proposals
out; this does not discard a successful proof. Receiving keys already reserved
remain in the wallet. A locked wallet may need unlocking to refill its keypool.

TLS capture uses the pinned Mbed TLS 3.6.7 library. Only ClientHello.random is
replaced by the claim challenge; ephemeral keys always use fresh randomness.
The proof contains the transmitted ClientHello and decrypted handshake messages
through CertificateVerify. No HTTP request or private TLS session keys are stored.
The supported profile excludes HelloRetryRequest, client authentication and session
resumption; a server outside the profile can be skipped with a diagnostic. Neither
the system root store nor local wall clock substitutes for Core's immutable roots
and chain median time. Capture alone is never evidence of a valid claim.

`wallet_p2c_auto_claim.py` runs offline in CI. Its optional `--live-domain=example.com`
flag explicitly enables an external TLS smoke test that creates, claims and confirms
a bounty on an isolated regtest node; it is never enabled by the standard runner.
