# Change capture

**What.** `capture::open(workspace, options)` watches a local workspace and appends one drip
for each completed, coalesced set of writes. Capture is enabled when opened. The workspace-local
control data is under `.liquid/capture/`: `drips.cbor` is the append-only journal, `cursor`
records its tail plus a platform token, and `state` is the most recent filesystem snapshot.
Subscribers read the journal from a cursor and can receive an already appended drip again after
a restart.

**Why.** Filesystem notifications can be coalesced, lost, or delayed. They are prompts to
reconcile the workspace against the saved snapshot, never the authority for history. The journal
append is the durable point; reading, digest work, storage, and rescans run outside the writing
application's path through the runtime's blocking-work adapter.

## Operation

Capture recognizes a temporary-file rename by reconciling its final target, so the temporary
path does not appear in history. File identity is derived from the target path and remains stable
across this form of save. A batch of paths found in one configured coalescing window is one drip
with multiple operations. Before append, every batch crosses the private `drip_filter` stage. Its
default accepts every batch; liquid #50 replaces that filter with inline secret scanning before a
drip can be journaled.

`capture_options::exclude` is the declared glob list for paths that must not drip.
`coalesce_window` defaults to 200 ms. `attribution` is present for Phase 2 and remains false;
foreign build output is governed only by declared exclusions in this phase.

`capture_status` reports running or paused state, queued work, overflow count, recrawl count,
and the latest public journal cursor token. Events are hints, so overflow and recrawl must be
visible rather than silently repaired. `pause()` retains the observed window without appending;
`resume()` reconciles that window and replays it.

## Platform mechanisms

| Platform | Mechanism | Capture behavior |
|---|---|---|
| macOS | FSEvents with file events and configured latency | `MustScanSubDirs`, kernel/user drops, and root changes trigger reconciliation. |
| Linux | recursive inotify watches | New directories are watched; directory moves and `IN_Q_OVERFLOW` trigger reconciliation. |
| Windows | `ReadDirectoryChangesW` on the root with subtree | A zero-byte successful completion is an overflow and triggers enumeration. |

The platform callback only adds a hint to the mutex-protected capture queue, then posts a tiny
wake coroutine on the shared runtime context. That runtime-side coroutine sets the async event;
OS threads never touch Capy primitives or await work. The capture coroutine waits without polling
while the workspace is idle; after its first hint, it starts the configured coalescing delay and
processes that batch. The coroutine-owned capture path performs reconcile, filtering, storage,
metadata transaction, append, delivery, and cursor persistence.
macOS uses the system FSEvents dispatch queue; Linux and Windows use runtime blocking work for
their required wait. No platform type appears in a public header.

## Verification

`tests/test_drip.cpp` covers atomic rename, a forty-file batch, declared exclusions,
pause/resume, restart replay, and injected watcher overflow recovery. The overflow test confirms
the counter increments, a recrawl occurs, and changes made while events were unavailable are
found. `tests/crdt/test_journal.cpp` covers cursor reads. Run the static and shared commands from
the root:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON && cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
```

## Measurement

Measured on this workspace's macOS host with a 100-file workspace, one run per window. The
journal-append figures time only the durable `journal::append(record)` call. The visibility
figures include filesystem delivery, the selected coalescing window, reading, storage, metadata,
and notification:

| Window | Journal append p50 / p99 | Save to drip visible p50 / p99 |
|---|---:|
| 200 ms | 329 / 329 µs | 304 / 304 ms |
| 20 ms | 245 / 245 µs | 37 / 37 ms |

The single-digit capture-path budget applies to the journal append, not save-to-drip-visible.
The latter intentionally includes the coalescing policy and is reported separately.

## Open questions

- Linux `fanotify` privilege requirements remain unresolved; this implementation uses inotify.
- Windows USN journal support is deferred. The watcher has no persistent Windows journal token.
- Action attribution is Phase 2; `capture_options::attribution` reserves its control point.
- Journal retention and compaction remain coupled to metadata-history retention.
