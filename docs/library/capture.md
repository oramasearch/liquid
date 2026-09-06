# Change capture

**What.** `capture::open(workspace, options)` watches a local workspace and appends one drip
for each completed, coalesced set of writes. Capture is enabled when opened. The workspace-local
control data is under `.liquid/capture/`: `drips.cbor` is the append-only journal, `cursor`
records its tail plus a platform token, and `state` is the most recent filesystem snapshot.
Subscribers read the journal from a cursor and can receive an already appended drip again after
a restart.

`subscribe_from(cursor, callback)` replays journal records after that cursor in order and supplies
the cursor after each callback. Registration and replay share the journal lock, so an append cannot
fall between them. `subscribe(callback)` remains the from-origin convenience overload. Consumers
persist a cursor only after their own side effect succeeds; duplicate delivery is therefore valid
and expected.

`close()` cancels the platform watcher and waits for both the capture task and the watcher's
outstanding read to finish. No watcher offload remains after the close task completes.

**Why.** Filesystem notifications can be coalesced, lost, or delayed. They are prompts to
reconcile the workspace against the saved snapshot, never the authority for history. The journal
append is the durable point; reading, digest work, storage, and rescans run outside the writing
application's path through the runtime's blocking-work adapter.

## Operation

Before `open` returns, capture writes an excluded `.liquid-capture-cookie-<n>` in the workspace
and waits up to 100 ms for the watcher to report it, then removes it. It retries with a new cookie
for up to five seconds; if arming still fails, `open` returns `errc::unavailable` with the watcher
and workspace. Events before a reported cookie precede its report, so callers cannot write into
the interval before the watcher is armed. Capture recognizes a
temporary-file rename by reconciling its final target, so the temporary
path does not appear in history. File identity is derived from the target path and remains stable
across this form of save. A batch of paths found in one configured coalescing window is one drip
with multiple operations. Before append, every batch crosses the private `drip_filter` stage and
then the inline secret scanner. The scanner examines lines intersecting changed FastCDC chunks,
blocks findings by default, and writes the signed attestation before the drip can be journaled.
See [`secret-scanning.md`](secret-scanning.md).

`capture_options::exclude` is the declared glob list for paths that must not drip.
`coalesce_window` defaults to 200 ms. `scan` defaults to true; path-and-detector rules and the
literal line marker are in `scan_config`. `attribution` is present for Phase 2 and remains false;
foreign build output is governed only by declared exclusions in this phase.

## Write flow control

`repository_options::flow` is persisted as `config.flow`. Its `per_actor`, `per_document`, and
`per_repository` token buckets each contain `rate_per_s` and `burst`; `actor_overrides` replaces
the actor bucket for a matching TAAP id. A document key is the containing directory's stable
`node_id`, and the repository bucket is the local checkout's tenant scope. `max_update_bytes`
bounds one transaction's operation payload and `max_unacknowledged` bounds work beyond the
slowest daemon consumer.

Admission happens before a directory-document transaction opens. An exhausted bucket returns
`errc::busy`; `error::context` names `scope`, `key`, and `retry_after_ms`. An oversized update
returns `errc::invalid_argument` and names the configured limit. Neither rejection creates a CRDT
operation or a drip. Callers wait with `runtime::delay(retry_after_ms)` before retrying; elapsed
monotonic time refills the bucket without a worker thread.

The human-oriented defaults are 10,000 operations per second with a 10,000-operation burst at
each scope, a 64 MiB update, 4,096 unacknowledged drips, and 2,048/1,024 high/low watermarks. A
bounded agent session can use actor `{rate_per_s: 10, burst: 10}`, document
`{rate_per_s: 20, burst: 20}`, repository `{rate_per_s: 100, burst: 100}`, a 1 MiB update,
128 unacknowledged drips, and 64/32 watermarks.

`reconcile_interval` defaults to two seconds; set it to zero to disable this backstop. A ticker
on the capture context compares watcher-event counts after each interval. If no watcher hint
arrived, it adds a reconcile hint to the same queue and wakes the capture loop. A backstop
reconcile that finds changes increments `recrawl_count` and emits through the ordinary capture
path. Hints make capture fast; reconcile makes it correct. Pausing suppresses emission, not the
ticker.

`capture_status` reports running or paused state, queued work, overflow count, recrawl count,
blocked-drip count, the last safe block coordinates, and the latest public journal cursor token.
Events are hints, so overflow and recrawl must be
visible rather than silently repaired. `pause()` retains the observed window without appending;
`resume()` reconciles that window and replays it.

A completed blocked scan updates the local filesystem snapshot but does not append a journal
record or update the accepted chunk list. This records that the exact blocked bytes have already
been reported, so a delayed watcher hint or periodic reconciliation cannot publish the same block
again; a later filesystem change remains eligible for scanning.

## Caller-stated write bases

Repository API writes run the issue #86 stale-reasoning check before remote registration, content
storage, or a directory-document transaction. The check compares the caller's change and optional
journal cursor with the current head and signed repository drips. Written paths and `depends_on`
form the material read set. A refusal appends no drip and leaves the working tree unchanged;
`repository::explain_stale` reads historical and current blobs on the runtime and returns text
hunks without nested blocking. Filesystem watcher capture remains base-optional because it records
an already completed local filesystem write rather than accepting an agent write request.

## Platform mechanisms

| Platform | Mechanism | Capture behavior |
|---|---|---|
| macOS | FSEvents file events, `NoDefer`, 50 ms latency | `MustScanSubDirs`, kernel/user drops, and root changes trigger reconciliation; close stops, invalidates, releases, then drains its serial queue. |
| Linux | recursive inotify watches | New directories are watched; directory moves and `IN_Q_OVERFLOW` trigger reconciliation. |
| Windows | `ReadDirectoryChangesW` on the root with subtree | A zero-byte successful completion is an overflow and triggers enumeration. |

The platform callback only adds a hint to the mutex-protected capture queue, then posts a tiny
wake coroutine on the shared runtime context. That runtime-side coroutine sets the async event;
OS threads never touch Capy primitives or await work. The capture coroutine waits without polling
while the workspace is idle; after its first hint, it starts the configured coalescing delay and
processes that batch. It drains queued hints, clears the event, then drains again before waiting,
so a wake arriving during the handoff is retained. The coroutine-owned capture path performs
reconcile, filtering, storage, metadata transaction, append, delivery, and cursor persistence.
macOS uses the system FSEvents dispatch queue; Linux and Windows use runtime blocking work for
their required wait. No platform type appears in a public header.

## Verification

`tests/test_drip.cpp` covers atomic rename, a forty-file batch, declared exclusions,
pause/resume, cursor-aware restart replay, and injected watcher overflow recovery. The overflow test confirms
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

With inline scanning enabled, the current 100-file Release measurement at a 20 ms window records
130.386 µs of scanner work, 220.917 µs for the append, and 43.9578 ms until subscriber visibility.
See the complete command output and interpretation in
[`secret-scanning.md`](secret-scanning.md#verification-and-measured-cost).

The single-digit capture-path budget applies to the journal append, not save-to-drip-visible.
The latter intentionally includes the coalescing policy and is reported separately. If a watcher
hint is lost, visibility is bounded by `max(coalesce_window, reconcile_interval)`.

## Open questions

- Linux `fanotify` privilege requirements remain unresolved; this implementation uses inotify.
- Windows USN journal support is deferred. The watcher has no persistent Windows journal token.
- Action attribution is Phase 2; `capture_options::attribution` reserves its control point.
- Journal retention and compaction remain coupled to metadata-history retention.
