# P2C in the wallet

The **P2C** tab (Alt+5) has separate **Create bounties** and **Automatic claims**
pages. Creating bounties uses the same type-2 outputs as `sendtop2c` and never
makes an HTTPS connection. Automatic claiming is disabled until explicitly enabled.

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
7. Confirm **Send P2C**, or cancel without sending. Submission IDs appear on the page.

In transaction history, each P2C output shows `P2C: domain` in the Label column.
The domain comes directly from that output, including for previously created or
RPC-created transactions. It is searchable and included when copying labels or
exporting the history; it is not an editable address-book entry.

Large requests are split into standard-sized transactions with independent confirmed
inputs. Enough total balance alone may not be sufficient: a split also needs enough
separate confirmed inputs. Inputs are temporarily reserved while awaiting approval;
cancellation releases those reservations without unlocking unrelated user-locked coins.
The exact signed transactions shown during review are submitted, without regenerating fees.

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
else. It reserves a new P2PK receiving destination in this wallet and deducts
fees exclusively from the bounty: no other wallet coins are spent. Local-key
wallets may remain locked if their keypool has an available receiving address.
Watch-only/external-signer wallets are not supported by these initial RPCs.

The preparation result supplies the canonical on-chain domain, work target,
root version, tip median time, fixed transaction hex and exact ClientHello
challenge. A proof generator must use these values unchanged. `proof_size`
defaults to the full 65536-byte consensus limit and determines the fee budget,
including transaction/witness framing. An explicit smaller budget can reduce
fees, but a larger actual proof may fail the current mempool minimum. Unused
budget remains a fee. Fees cannot be changed after generating the proof
without generating a new challenge and doing the TLS work again. The optional
`fee_rate` is in connects/vB, consistently with other wallet RPCs.

Submission checks that the payout belongs to this wallet, the input is a
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
and optionally a comma-separated domain allowlist. Confirm **Apply / start**.
Successful proofs are submitted automatically, without a separate redeem click.
The wallet needs local keys and `walletbroadcast=1`, but no pre-existing balance.

The equivalent RPCs are:

```
connectcoin-cli -rpcwallet=claimant setp2cclaiming 1 4 '["example.com"]'
connectcoin-cli -rpcwallet=claimant getp2cclaimstatus
connectcoin-cli -rpcwallet=claimant setp2cclaiming 0
```

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
  any positive 32-bit integer, with no separate 64-connection cap. It is a maximum,
  not guaranteed throughput: OS thread/socket limits, memory, the selected rate
  and server responsiveness determine actual parallelism. Failure to allocate a
  connection worker stops the search with an error, joining already-started workers.
- An empty allowlist permits all supported public domains with confirmed bounties.
  HTTPS reveals your IP to those domains. Connections are direct, on port 443;
  private/unroutable addresses are excluded. A configured name, IPv4 or IPv6 proxy
  disables this path rather than being silently bypassed. SOCKS support is not
  implemented for the claim generator.

The worker scans a UTXO snapshot outside the chain lock after flushing its cache
and **rotates automatically between domains**. Guaranteed round-robin turns
alternate with extra turns for the domain with the highest expected net return.
Economic preference does not move the round-robin cursor: even a high-reward
server that never supplies a usable proof cannot prevent the other domains from
being tried. For example, with three eligible domains A, B and C and A as the
economic winner, turns are A, A, B, A, C, A, then repeat. This deliberately balances
economic preference with fair exploration; it is not proportional allocation.
The economic winner is recalculated between turns using its best available
bounty, not the sum or count of its outputs. Locked or mempool-spent outputs do
not win extra turns, and unusable proposal windows are skipped as usual.
Skipping an unusable fair domain does not grant an extra economic turn.

There is **no per-domain connection quota or configurable per-domain limit**.
Only the wallet-wide rate and concurrency settings limit traffic. Internally the
scheduler uses a 30-second search quantum to revisit other domains, ending early
when the current domain's eligible bounties are exhausted (or on stop/fatal error).
The quantum starts after discovery, proposal preparation and the first DNS lookup,
so a slow discovery cannot repeatedly expire a turn before any TLS attempt.
Further proposal windows and DNS lookups in that turn do not reset its deadline.
This is not a quota of connections per 30 seconds: there is no attempt cap or
cooldown, and a sole eligible domain resumes immediately with the wallet's full
configured capacity. OS DNS, cancellation and saving/submitting finished work may
delay handoff; this is not a hard wall-clock shutdown guarantee. `domain_rounds`
is only a diagnostic count of rounds since the latest configuration.

A successful proof finishes only that bounty: its duplicate attempts are cancelled,
its proof is saved and submitted, and other bounties continue in the same round.
This scheduling protects against monopolization by an individual domain, not an
attacker controlling many distinct domains. Nor can it force a server to provide
winning TLS transcripts. Claim generation remains independent of node consensus.

Connections are shared round-robin among that domain's bounties, with a rotating
window of up to 256 proposals. Exhausting a window refills it from the same domain
without resetting the deadline or rotating away prematurely. Within each
domain, the rotation is ordered by descending expected net return per valid
connection: `(target + 1) / 2^256 * (reward - claim_fee)`. The inclusive `+1`
matches consensus's `hash <= target`. Ranking uses exact 320-bit integer
numerators, without floating-point rounding or overflowing at the maximum
target. Existing proposals use their fixed payout/fee; new proposals use the
same full-proof fee calculation as claim construction. This priority applies
when selecting the window, not just after selecting outputs by transaction ID.
Equal priorities use outpoint order. The cursor advances through this ranked
cycle, preserving rotation rather than monopolizing all rounds with one bounty.
This changes search priority, not rewards or consensus rules. Each attempt
uses its own bounty-specific claim challenge; proofs are never reused for other
outputs. More bounties alone do not buy extra domain turns. Output cursors
also rotate after partially completed rounds so low rates cannot starve later
bounties. These are rounds, not additional per-domain connection-rate settings:
if only one domain remains, its next round can start immediately.
When a round finishes with eligible work,
it rescans and rotates immediately, without an idle delay. DNS failures back off
for two seconds; failed TLS attempts and rejected certificates back off for one
second per connection worker. All these waits are interruptible when stopping.
If no matching bounties exist, the state is `waiting for bounties`; if matching
bounties exist but no domain has searchable work (for example, all are locked or too small
to pay the claim fee), it is `waiting for eligible bounties`. Both idle cases
rescan after five seconds to avoid a busy loop and discover new eligible work.
These are scheduling/memory bounds, not a lifetime attempt limit: difficult
bounties are tried again. This implementation has no persistent bounty index;
on a large UTXO set, discovery itself can take time. The node remains responsible
for rechecking availability and all consensus/policy rules before acceptance.
The worker detects competing mempool claims and spent outputs, cancelling their
in-flight searches. TCP setup is bounded to one second and a TLS attempt to ten
seconds; cancellation is checked between steps. An OS DNS lookup may still delay
stopping until that lookup returns. HTTPS is independent of the P2P network toggle;
use **Stop HTTPS (0)** to stop it.

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
