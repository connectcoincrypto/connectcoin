# Qt backend work and responsiveness

The GUI thread owns widgets, Qt models, cached display state and confirmations.
It must not wait for chain/wallet locks to paint or process ordinary events.
This separation is independent of the miner's thread count or Secure JIT mode.

## Background work

| Area | Worker-side work | GUI-side work |
| --- | --- | --- |
| Wallet status | Balances, encryption/keypool/HD capability snapshots | Apply completed snapshots; coalesce notifications |
| Address book / receive | Initial database reads, address derivation, edits and receive-request persistence | Cached labels, rows and request IDs; enable receive after initialization |
| History / overview | Initial transaction decomposition, incremental transaction snapshots, bounded status batches, explicit detail/hex queries | Paint/filter cached rows; reject stale transaction revisions |
| Coin control / sending | UTXO lists and statistics, fee estimates, change ownership, transaction preparation/signing/commit and fee bumps | Selection, amount editing, review and explicit change-address consent |
| P2C | Claim status/configuration, bounty transaction preparation/signing/commit and reservation release | Form state, bounded proof-check progress and confirmation |
| Node / mining / network | Chain/mempool/network/miner snapshots, peer and ban lists, start/stop mining, ban/disconnect actions | Cached status, traffic graph and mining controls |
| Wallet management | Loading/creating/restoring/migrating, directory scans, signer enumeration, encryption and backup | Modal choices, registering models and detaching views |
| PSBT / messages / exports | PSBT parsing/signing/finalization/analysis, message signing/verification, file serialization/writes, CSV output and PNG saves | Snapshot Qt values, clipboard and file-dialog interaction |
| Options / GUI preferences | Serialized settings reads/writes, effective-settings snapshots, startup shortcut IO and NAT-PMP changes | Cache-only option/geometry/font values; enqueue coalesced writes |
| Payment IPC | Startup name/datadir resolution, stale socket cleanup and connection/write waits | Explicit post-construction local-server listen; bounded incremental `readyRead` processing |
| Startup / shutdown | Configuration reads/migration, translation catalogs and dependency lifetimes, base initialization, directory checks, node interruption, miner joins and shutdown commands | Choices, translation-facade installation, splash/progress display and deferred teardown |
| Console / diagnostics | Command parsing/filtering, complete command/reply HTML preparation, RPC execution (including `stop`), Qt/system diagnostic discovery and log writes | Input, clipboard, presentation-object snapshots and Qt document layout |

Passive refreshes keep at most one outstanding request of each type. Wallet
transaction/status batches are bounded to 64 entries. Workers receive copied
inputs and backend objects, not widgets or mutable GUI controls. Results are
applied on the GUI thread and checked against the current model, selection or
request generation where necessary. History status application additionally
limits each GUI turn to 128 output rows: bounding transactions alone does not
bound one transaction with many outputs. Same-tip invalidations record a forced
refresh once instead of scanning every output immediately. Continuations locate
the transaction's current row range and reject superseded revisions. Retained
history indexes resolve current row storage rather than keeping QList pointers.
Automatic-claim pages stop passive status polling while hidden/minimized,
including child pages of a minimized top-level window. Pending user configuration
still completes and actual background claiming is unchanged. Send pages coalesce
block-triggered fee refreshes while invisible and resume on display/restore.
Fee-display queries copy only confirmation-target/RBF/mode inputs, not the full
selected-UTXO/script set; transaction preparation retains its own complete state.

Balance and encryption/keypool invalidations are coalesced before posting to Qt,
not just before querying the backend. Transaction/progress events retain their
ordering but are drained in batches of at most 64; buffered rescan notifications
are similarly bounded. New transaction records are decomposed on the worker.
Status results invalidate only affected contiguous row ranges. Address labels
use canonical-text lookups on the paint path, with an explicit signal to refresh
label-based searches after an address-cache change.

Address-book bursts coalesce by canonical address before posting to Qt, retaining
the newest state for each address. Delivery and pending initial-snapshot replay
are limited to 64 addresses per turn; a newer live update supersedes old replay
data. Reopening an address selector reuses its normal or signing-only cache
instead of creating another full snapshot and permanently adding a notification
recipient. Both cached views continue to receive live changes. Address lookup uses
binary search in the sorted cache, and persistent indexes resolve their current
row instead of retaining pointers into movable QList storage. Wallet-directory
snapshots arrive already ordered from the existing node worker; the GUI only
combines them with its current loaded-wallet state.
After address-book mutations, the cache reads back authoritative backend data
instead of reinstating the submitted label or deletion. Per-address notification
revisions reject a readback superseded while the GUI waits. This preserves later
RPC edits, including an address recreated after deletion. Regression callbacks
queue later writes after the original database transaction commits; they never
reenter a write from a notification emitted inside that transaction.
Label invalidation scans at most 128 history rows per turn and emits only
matching contiguous ranges. Its cursor follows structural changes and restarts
for newer label changes, without a single full-history GUI callback.

New blocks repaint visible confirmations instead of invalidating every history
row and triggering a full proxy refilter. Changed transactions eagerly refresh
their status even when filtered out. Conflict and disconnect transitions notify
the affected wallet transactions and descendants, so hidden conflicted rows can
return correctly after a reorganization. The overview exposes at most five rows
through a bounded proxy; it no longer hides or measures every history row to
display five. Explicit transaction focus uses binary lookup in the hash-sorted
cache instead of a model-wide match that requested every row's status. Focusing a
multi-output transaction applies one selection and scrolls only to its endpoints.
It preserves source-output selection order and excludes unrelated interleaved or
filtered rows rather than selecting their bounding range.
Depth-only status snapshots use the existing `confirmationsChanged` viewport
repaint signal, once per bounded application turn. Tooltips and plain-text copy read the refreshed
cache on demand. A role-limited `dataChanged` is not sufficient here: Qt still
rechecks proxy sorting/filtering for those rows. Actual state, balance-eligibility
and sort-key transitions continue to emit `dataChanged` with the corresponding
roles, preserving conflict filtering and recovery.

Peer refresh and result timers stop outside the peers tab or while its window is
hidden/minimized, and resume on visibility. Unchanged node metadata does not
rebuild proxy/local-address presentation on every status poll. Peer resource
icons are recolored on palette changes, not on every refresh. Help options are
loaded off-GUI only on demand, and their document is laid out as one edit. On
Linux, notification-service detection uses an asynchronous DBus probe instead
of synchronous remote introspection (runtime validation here was on Windows).
Connection/network/alert/ban invalidations also merge before Qt posting, not
merely before querying the backend. Startup logging/parameter setup and RPC
history deduplication execute on workers; neither needs widget ownership.
Core verification/replay progress coalesces repeated percentages and pending
intermediate values before Qt posting. Start/completion boundaries and operation
order are retained, with at most 64 deliveries per GUI turn.
Wallet rescan progress uses the same bounded delivery rule independently of the
node's progress channel. Reentrant stop closes delivery even midway through a
batch; it does not let remaining values reopen a stopped progress dialog.
The transaction table only needs transitions into/out of rescan buffering; its
notification queue suppresses unchanged buffering states before Qt delivery,
including repeated intermediate percentages, while preserving transaction order.
Startup network-logo decoding, recoloring and scaling use QImage on a worker;
only the final QPixmap/QIcon objects are constructed on the GUI thread.
Consecutive peer departures use one removal range instead of one list shift and
proxy update per peer. Persistent peer indexes resolve the current snapshot by
row, never a pointer into the replaced snapshot's storage.

Core signal disconnection does not wait for callbacks that have already started.
Wallet, transaction-table, client and splash notifications therefore use shared lifetime
gates: teardown clears the target under the same short mutex used to enqueue
notifications. The gate never remains locked while executing GUI work. A late
callback can be discarded without dereferencing a destroyed model.

Transaction `data()` reads never enter a nested event loop. Details and raw hex
are explicit asynchronous requests with copied transaction identities. Recipient
validation, selected-coin balance, transaction construction, fee redistribution
and virtual-size calculation run together on the wallet worker; confirmation
dialogs read the cached size, including after external signing.

CSV export snapshots at most 128 rows per GUI turn because Qt models cannot be
read from another thread. A model revision change restarts capture, with at most
three retries. If it never stabilizes, export fails before opening the destination;
it does not silently mix rows from different revisions. One owned export worker
accumulates the complete snapshot, discards old revisions and encodes/writes the
file. The GUI hands off at most two commands/chunks of up to 128 rows, so growing
or reclaiming the complete export never becomes a single GUI operation. Worker
cancellation and cleanup complete before the explicit export action returns.
Coin-control menus only redraw a deferred changed snapshot, not on
every copy/menu dismissal. Initial receive-history sorting also runs off-GUI.
Recipient summaries/totals, PSBT filename suggestions and P2C receipt formatting
run on copied worker-side values. Multi-recipient sends take one wallet-count
snapshot rather than querying the loader once per recipient.
Receive-history user sorting and bulk-deletion matching also use worker snapshots.
Revision checks rebase results after intervening edits; grouped removals and real
layout notifications preserve surviving persistent indexes. Retries are capped
at three. Continuously reentrant edits leave sorting to one asynchronously polled
latest-state snapshot; they never move full-history sorting back onto the GUI.
Bulk deletions retain a single current-state GUI matching scan only if all three
rebase attempts were invalidated: already-erased database records must disappear
from the view. This exceptional deletion path is not an ordinary refresh or
new-block operation.
Coin-control group checkbox changes coalesce fee-label refreshes before copying
the selected-coin set, avoiding one growing snapshot per child. Lock icons are
recolored once per render, and lock/unlock/menu continuations are guarded against
dialog destruction during responsive backend waits. The PSBT save prompt is
heap-owned so its page's teardown cannot delete an object on the caller's stack.

GUI preferences use one application-owned worker and an in-memory cache, with
network/format identity captured when each facade is constructed. Reads and
widget destructors never touch storage. Writes coalesce per key; failed batches
are retained for a later write or explicit flush, without a busy retry loop.
Startup/profile reload is an explicit responsive operation and preserves edits
made while a read was in flight. Normal shutdown snapshots widgets then flushes
before stopping logging. Storage errors are logged, not shown as modal dialogs.

Qt diagnostics use one owned worker, with a queue limited to 1,024 messages and
1 MiB, and a limit of 8,192 characters per message. Saturation discards messages
and produces a count report rather than blocking the GUI or growing without
bound. Fatal diagnostics retain synchronous emergency logging because Qt aborts
after the handler returns. Startup Qt/system diagnostics snapshot Qt-owned
presentation objects first, then perform system discovery and direct Core log
writes on a worker while the event loop remains responsive. Ordinary Core logging
outside these GUI diagnostic paths is unchanged.

Explicit actions may use `GUIUtil::WaitForBackendTask`: the backend operation
runs in a worker while a non-cancellable dialog keeps the Qt event loop alive.
This helper is not for paint/data refreshes or destructors. An outer
`BackendOperationGuard` protects actions spanning unlock, confirmation and
result handling. Closing its progress dialog does not cancel a transaction.
The stack-owned progress dialog is not a QObject child of its caller: queued
caller destruction cannot delete that stack object or end the wait early.
Explicit action continuations guard their widget lifetime before applying results.

## Shutdown and lifetime

Wallet unload/shutdown defers destruction during guarded actions. Normal
teardown detaches views, calls `WalletModel::stopWorker()` while the model is
still alive, and only then deletes it. `ClientModel::stopWorkers()` likewise
stops producers before draining node/peer/ban workers. Queued callbacks observe
stopped state. Thread-pool draining itself runs off the GUI thread because
`ThreadPool::Stop()` can execute queued jobs in its caller.

Destructors retain synchronous safety fallbacks for exceptional/nonstandard
ownership, but must never start nested Qt event loops. P2C page destruction
queues owned reservation cleanup for the wallet worker; model teardown drains
that work before releasing the backend. `WalletModel::destroy()` deletes the
quiesced Qt model first and releases its backend reference on a worker, so final
claim-worker/database cleanup cannot block the GUI. Controller registration never holds
its mutex across a blocking GUI invocation.
`WalletController::stop()` closes the shared loader gate before draining activity
and registration work. A loader awaiting GUI registration is cancellable during
teardown; normal model destruction happens while the controller is still alive.
Its destructor's exceptional fallback does not enter another GUI event loop.
Core message/question delivery similarly retains a shared gate, not a raw window
or caller-stack result pointer. Unsubscribe cancels pending modal replies, even
before their GUI event is dispatched, without holding the gate across a dialog.

Node interruption is submitted once and awaited without blocking event delivery;
teardown also waits for initialization and active GUI operations to unwind.
Splash teardown closes its callback gate and drains already-entered loader
registrations before disconnecting subscriptions and releasing backend references.
The initial directory dialog resolves/caches the OS default directory on a worker,
waits asynchronously for a pending disk probe before completing, and supports
reopening after a directory-creation error. Coin-control dialogs are owned by their
sending page, so neither timers nor raw coin-control references survive its deletion.

## What deliberately stays on the GUI thread

Widget construction, painting, Qt model notifications, applying snapshots,
selection/clipboard handling and confirmation dialogs belong on the GUI thread.
Cheap immutable/atomic flags and short copy-only GUI/proxy locks do not perform
wallet/chain scans or wait for filesystem writes. Qt-native dialogs still use
the platform's normal APIs. Qt document layout and proxy-model ordering remain
on the widget-owning thread; copied value-only lists can be sorted by workers.
Console font changes update the document's
default font without serializing/replacing the entire history or losing selection.
The pending-command indicator is separate from history/undo state. Clipboard,
large layouts and final destruction of cached value containers still require
GUI work; moving those Qt-owned objects directly to a worker would be unsafe.
Translation files, including private `.qm` dependencies, are owned by one
persistent worker from loading through destruction. An installed GUI-owned
facade delegates only immutable, thread-safe `QTranslator::translate()` calls.
Reload leaves the old catalog active until the new one is ready; unregistering
the facade drains Qt translation readers before replacing its catalog pointer.
Old catalogs are reclaimed on the worker. Normal shutdown unregisters and joins
responsively; bootstrap/error destruction has a synchronous safety fallback
without a nested event loop. Loading/stopping cannot reenter another load.
Single-item copy/availability, history actions and receive-selection checks stop
at the first fully selected row, without creating indexes for the entire
selection. Contiguous receive deletion counts range endpoints instead of
materializing every selected index. Address-entry validation
compacts pasted whitespace in one pass, not one string shift per character.

This is a responsiveness refactor, not a guarantee that CPU saturation, memory
pressure, a graphics driver or extremely large Qt layouts can never cause a
visible delay. It does not change mining priority, thread defaults or consensus.
The separate RandomX policy is described in `cpu-mining.md`.

## Regression coverage

The Qt tests exercise held `cs_wallet`, `cs_main` and settings locks while GUI
timers, cached queries and rendering continue. They also cover initial model
loading, transaction delete/re-add revisions, hidden receive pages, stale
coin-control results, worker draining, rejected progress-dialog cancellation,
exception propagation, signing/relocking and ordinary wallet/P2C workflows.
Payment IPC tests cover partial/prebuffered frames, invalid lengths, truncated
disconnects and exactly-once delivery.
Additional regressions cover blocked startup/shutdown settings access, stalled
directory-check completion, bounded logging under a stalled sink, coalesced
notification bursts, rescan/live-event ordering and label-filter invalidation.
The third pass adds explicit transaction-request lifetime checks, coherent bounded
CSV capture, transaction-preparation/size-cache tests, blocked preference I/O with
profile isolation and retry, coin-control parent lifetime, console clear/font during
a pending RPC, and splash shutdown during a blocked wallet-loader registration.
The fourth pass covers the five-row overview boundary and source destruction,
sparse conflict/descendant/orphaned-coinbase updates and disconnect recovery,
binary transaction lookup, hidden/minimized peer timers, unchanged node metadata,
lazy help with a held settings lock, caller deletion during backend waits,
wallet-controller registration cancellation, and core-message delivery after
caller return or window teardown. PSBT/signing regressions exercise deletion of
their requesting dialog while a queued backend operation is still blocked.
The fifth pass adds held-settings-lock startup parameter setup, two bursts of
8,000 node invalidations, RPC history duplicate ordering, bounded address-book
delivery and initial replay with newer edits, address-cache reuse, and chunked label invalidation
under sparse matches, row removals and further renames.
The sixth pass covers confirmation-depth repainting without proxy comparisons,
2,048-peer batch removals and persistent indexes, receive-history sort/removal
while edits and GUI timers continue, first-item copy with 100,000 selected rows,
and linear address-input cleanup. Existing send/P2C tests cover the moved summary
and receipt preparation.
The seventh pass adds a 320-output transaction with bounded status delivery,
same-tip conflict/recovery, a newer update between chunks, and retained history
indexes across 640-row insertion/removal. It also forces three receive-sort
invalidation races before an asynchronous retry, checks a 256-output coin-control
group produces one label-refresh event, and deletes coin-control dialogs while
lock/unlock is blocked. Structural model insertion/removal, proxy ordering and
Qt layout remain atomic GUI-owned work; these tests do not assert a global
latency bound for arbitrarily large models.
The eighth pass checks 513-address binary lookup without model data scans,
persistent address indexes across insertion/removal, ordered asynchronous wallet
directory snapshots, and one-shot multi-output transaction focus under both sort
directions and partial filtering. It also exercises startup diagnostics with a
stalled log sink and verifies literal markup, ampersands and UTF-8 survive RPC
command/reply worker formatting without double escaping.
The ninth pass adds hidden/minimized claim and send-page checks (including
hidden pending configuration completion), per-block progress floods and bounded
operation-boundary delivery, and pixel comparisons against the former network
icon pipeline. Translation tests stall the actual worker during load/reload/stop,
reject nested lifecycle operations, exercise concurrent Qt translation readers,
and compare real Portuguese/Arabic catalogs, plurals, relative QM dependencies,
filename fallback and missing-catalog behavior. Application tests also exercise
the owned translator's normal startup/shutdown path.
The tenth pass exercises separate wallet/history rescan floods, preserved operation
boundaries, joining a rescan at an intermediate percentage, reentrant progress and
stop during delivery. Payment startup tests
hold the actual settings lock during local-server preparation and command-line
forwarding, with duplicate-start and target-destruction checks. CSV tests extend
coherent bounded capture to late revision changes and source destruction. These
export/startup/rescan fixes do not establish a cause for ordinary mined-block
taskbar stalls or replace a native before/after latency measurement.

Run from the repository root:

```text
cmake --build build --config Release --target connectcoin-qt connectcoin-test-qt connectcoin-test --parallel 4
ctest --test-dir build -C Release -R "^connectcoin-test-qt$" --output-on-failure
ctest --test-dir build -C Release -R "^(wallet_tests|interfaces_tests|threadpool_tests)$" --output-on-failure --parallel 2
```

Tests use temporary regtest wallets, not the user's wallet. Programmatic Qt
minimize/restore and event-loop checks do not reproduce every native Windows
taskbar/driver timing condition.
