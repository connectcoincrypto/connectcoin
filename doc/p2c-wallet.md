# Creating P2C bounties in the wallet

The **P2C** tab (Alt+5) creates the same type-2 outputs as `sendtop2c`.
It does not collect TLS proofs, make HTTPS connections, or automatically claim rewards.

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
