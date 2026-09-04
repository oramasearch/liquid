# Change capture

> Research: [16-monorepo-scale](../research/16-monorepo-scale-and-authoring-loop/) ·
> [21-consumer-sync-and-web](../research/21-consumer-sync-and-web/)
>
> How a write to disk becomes a **drip** ([02](02-version-control.md)). Auto-drip is
> **on by default.**

## The primary mechanism is not inotify

The intuitive design is "watch the filesystem with inotify and record what changes."
That is the **fallback**, not the mechanism.

liquid provides the virtual filesystem ([01](01-object-model-and-storage.md)), so
writes pass **through** liquid on their way to disk. It does not need to be told what
changed — it performed the change. Like EdenFS, liquid keeps a **journal** of modifying
I/O, so "what changed since X?" is answered by `changed_since(cursor)` in
**O(changed files)** rather than O(repository files).

```
Editor writes a file
        │
        ▼
liquid VFS  ──→ chunk the write, journal it, emit a drip
        │
        ▼
disk
```

This is why EdenFS-backed Sapling `status` is fast on repositories where `git status`
is not, and it is available to liquid for the same reason: **liquid owns the
filesystem.** A tool that only observes from outside can never do better than the OS
event stream allows.

**OS watchers cover what the VFS does not** — files outside a liquid workspace,
non-materialised paths, and platforms or configurations where the VFS is unavailable.

In the Phase 1 FUSE implementation, create, write, move and remove requests call the repository
mutation API. Its persisted journal records the operation and stable `node_id`; no OS watcher sits
between the mount and a drip. The capture subscription and watcher fallback remain owned by their
separate issues.

## What OS watchers actually give you

Each platform is different, and the differences are not cosmetic.

| Platform | Mechanism | The reality |
|---|---|---|
| **Linux** `inotify` | Per-directory watches | **Not recursive.** You must walk the tree, install a watch per directory, watch for new subdirectories, and re-install after renames. Limits `max_user_instances`, `max_user_watches`, `max_queued_events` — commonly **128 / 8,192 / 16,384**. Overflow drops events and emits `IN_Q_OVERFLOW`. Installing watches on a huge tree is itself a long unprotected scan. |
| **Linux** `fanotify` | Mount or filesystem marks | Avoids one watch per directory — the reason to prefer it at scale. But it is aimed at antivirus and audit, not IDE watching: broad marks may need `CAP_SYS_ADMIN`, path reconstruction is harder, and it still overflows (`FAN_Q_OVERFLOW`; a 16,384 hard limit before Linux 5.13 without privileged unlimited queues). |
| **macOS** FSEvents | Daemon journal with persistent event IDs | Coalesces by design via a latency parameter. **Events may name a directory rather than each changed file**; `MustScanSubDirs` means recursively reconcile that subtree. `KernelDropped` / `UserDropped` requires a full scan. |
| **Windows** `ReadDirectoryChangesW` | Directory handle, optional subtree | A fixed-size kernel buffer is allocated on the first call and **never resized**. On overflow the contents are discarded, the call may return success with **zero bytes**, and you must enumerate the tree. |
| **Windows** NTFS USN journal | Persistent per-volume record sequence | **The best option for restart and recovery**: save a cursor, read changes since. But it is volume-wide rather than repo-rooted, so you map file IDs and parents and filter paths. Journal wrap or a mismatched journal ID forces reconciliation. NTFS only. |

**The choice per platform:** liquid's own journal first; then USN journal on Windows,
FSEvents on macOS, and `fanotify` on Linux where privileges allow, falling back to
`inotify` with tuned limits.

**8,192 watches is the number that matters.** A monorepo has far more directories than
that, which is why the JetBrains "inotify watches limit" support article exists and why
inotify alone cannot be the plan.

## Events are hints, not truth

The single most important correctness rule, and where naive watchers lose data:

> Queues overflow, events coalesce, mounts disappear, and the client may be offline.

So liquid **always** maintains:

- a **persisted cursor** (its own journal position, or a USN/FSEvents event ID);
- the ability to **rescan and reconcile against its database** — after overflow, after
  restart, after a mount change, on demand;
- **explicit overflow counters and recrawl telemetry**, because silent recrawls that
  report "everything changed" are how users lose trust.

Watchman's approach is the precedent: on queue loss it deliberately recrawls and may
report many files as changed. Correct, and conspicuous.

For "have I seen everything up to now?", **cookies** work: write a sentinel file into
the watched root and wait to observe it, since earlier events precede it in the stream.
Note the limit — cookies establish **ordering, not durability** against dropped events,
and are unavailable during an initial crawl.

## When is a write finished?

A close event is not universally available, and applications do not write files the way
you would expect. The dominant pattern is **atomic save**:

```
write  .foo.swp.tmp   →   fsync   →   rename over foo.c
```

Naively that reads as *delete `foo.c`, create an unrelated temp file, delete the temp*.
It must be recognised as **one completed write to `foo.c`.**

liquid's rules:

- **Prefer a real completion signal** where one exists — `IN_CLOSE_WRITE` on Linux is
  exactly this — while knowing it does not fire for `mmap` writes or files held open.
- **Recognise the rename-into-place pattern** as a save, not a delete plus create. The
  datastore's stable `ObjId` ([17](17-implementation.md)) keeps the file's identity
  across it, so history is continuous rather than a death and a birth.
- **Debounce, and verify stability** — size and mtime settled — before hashing.
- **Hash a consistent snapshot.** A blocklist assembled from two different moments of a
  write is corrupt, and it will not be obvious later.
- **Retry rather than conclude.** A locked or temporarily unreadable file is not a
  deletion.

## What gets dripped — and the advantage nobody else has

Auto-dripping everything means versioning `node_modules`, `target/`, `.cache` and every
`.o` file. That is fatal, and `.gitignore`-style pattern matching is a weak defence
because it is guesswork maintained by hand.

**liquid does not have to guess.** Build actions run in a sandbox and liquid observes
exactly what each one reads and writes ([04](04-build-engine.md),
[05](05-caching.md)). Therefore:

> **A write produced by a liquid action is a build output. A write not attributable to
> any action is authored work.**

Generated files are excluded *because liquid watched itself generate them*, not because
someone remembered to add a pattern. This is a genuine structural advantage — Dropbox
cannot do it, and neither can git.

Declared rules remain, for foreign build systems liquid did not run and for content the
user simply does not want versioned.

## Coalescing

A build writing 10,000 object files must not produce 10,000 drips.

- **Attribution first.** Action-attributed writes are outputs and never drip.
- **Time-windowed coalescing** for authored writes, so a formatter touching forty files
  on save is one drip, not forty.
- **Causal grouping.** Writes within one editor save operation are one drip.

The tunable is the window; the default should favour "one save, one drip."

## Every drip is signed

Capture and attribution are the same step. A drip carries a
[TAAP/1](../standards/taap-1-spec-draft.md) signature from the actor that produced it —
human or agent — so authorship is a recorded fact rather than an inference from
metadata. See [25](25-identity-attestation-and-transparency.md).

The performance consequence is real and is called out there: signing sits on the save
path, and the budget is single-digit milliseconds. Batching signatures across coalesced
drips is the likely answer.

## The fan-out

A drip is not only a history record. **It is the event that drives the system.** One
capture, many consumers, each subscribing to the drip stream:

```
                        ┌──→ local build graph      invalidate + recompile
                        │
                        ├──→ remote execution       distributed compile
   write ──→ drip ──────┤
                        ├──→ search index           local, and/or lake.live
                        │
                        ├──→ collaborators          CRDT sync
                        │
                        ├──→ lake.live              review, preview env, CI
                        │
                        └──→ running processes      hot update, where classified safe
```

### Why the drip is cheap to send

This is what makes the fan-out affordable rather than absurd.

Everything is content-addressed, so **the remote side already has every chunk except
the ones that just changed.** A drip is a journal entry plus a handful of new chunks —
kilobytes, not a tree. There is no upload of context, because the context is already
there under its hashes.

The consequence is that **speculative remote compilation on every save becomes
economically reasonable.** Send the drip, let the farm race ahead on the affected
actions, and by the time the developer asks for a result it may already exist. Existing
systems cannot do this because they would have to ship a workspace to find out.

### The consumers

| Consumer | What the drip triggers |
|---|---|
| **Local build graph** | Invalidate the minimum set of queries; recompute what is demanded ([04](04-build-engine.md)). |
| **Remote execution** | Dispatch newly-invalidated actions to the farm ([06](06-distributed-execution.md)). Racing local against remote decides which answer arrives first. |
| **Search index** | Reindex the changed blobs only — delta segments and tombstones, never a full reindex ([12](12-search.md)). Local index always; lake.live's index if the workspace is pushed. |
| **Collaborators** | The drip *is* a CRDT change, so sync is the same mechanism whether the write came from this keyboard or another ([15](15-consumer-sync-and-web.md)). |
| **lake.live** | Change review updates, preview environments rebuild, checks run ([18](18-lake-service.md)) — with no CI configuration, because the build graph already knows what is affected. |
| **Running processes** | Where the change classifies as hot-appliable, patch in place; otherwise roll ([14](14-hot-update.md)). |

### Delivery rules

Fan-out must never make saving slower or riskier:

- **The journal append is the only synchronous step.** Every consumer is fed
  asynchronously from the journal, by cursor.
- **Consumers are independent and resumable.** A stalled search indexer must not block
  the build; each keeps its own cursor and catches up.
- **At-least-once, idempotent.** Consumers may see a drip twice after a restart.
  Because actions are pure and indexing is by content hash, replay is harmless.
- **Backpressure is local.** A slow or offline consumer falls behind; it does not
  propagate pressure back to the editor.
- **Offline is normal.** Remote consumers — the farm, lake.live — are accelerators.
  Their absence slows things down and breaks nothing.

## Secrets are now on the hot path

This is the hazard auto-drip introduces, and it deserves to be stated plainly.

Under git, a credential pasted into a file and deleted before committing never enters
history. **Under auto-drip it was written to disk, so it was captured.**

Therefore:

- Secret scanning runs **inline on the drip path**, not at commit time
  ([11](11-secrets-and-encryption.md)).
- A detected secret **blocks the drip** by default and raises it immediately, rather
  than recording it and warning later.
- Redaction must genuinely remove the content from the drip history, and the tool must
  say what it did.

Getting this wrong turns a convenience feature into a credential-exfiltration
mechanism. It is the single riskiest consequence of auto-drip.

## Local by default

Automatic capture of every write is only comfortable if the user controls where it
goes.

- Drips are **local until pushed.** Capture is not publication.
- The workspace can be paused (`liquid pause`) — with the state visible, not silent.
- Nothing leaves the machine without an explicit push or an explicitly configured sync.

## Performance budget

The capture path is on every save, so it must be invisible: **single-digit
milliseconds** for an ordinary source file, and never blocking the writing application.
Hashing and chunking happen off the critical path; the journal append is the only
synchronous step.

If capture is perceptible, users will turn it off, and every feature built on drips
degrades with it.

## Open questions

- **fanotify privileges.** Mount-wide marks may require `CAP_SYS_ADMIN`, which is a
  hard sell for a developer tool. Whether liquid ships a small privileged helper, or
  accepts inotify limits outside its own VFS, is undecided.
- **Drip granularity.** Per completed write is the default. Per keystroke is possible
  via the editor ([21](21-liquify-ide.md)) but its metadata cost is unproven.
- **Coalescing window default**, and whether it should adapt to observed editor
  behaviour.
- **Attribution for foreign build systems.** `liquid wrap` gives attribution; a build
  run outside liquid does not, and falls back to declared rules.
- **Journal retention** before compaction — the same CRDT history-growth question as
  [17](17-implementation.md), which auto-drip makes more acute rather than less.
