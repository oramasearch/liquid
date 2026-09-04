# Store garbage collection

**Status: implemented for single-process local stores (liquid #8, #46).** Leased roots, epoch
marking, chunk-list reference extraction, whole-pack deletion, sparse-pack compaction, dry runs,
and retained-chunk explanations are available through `liquid/store.hpp`. Repositories retain their
own checkpoint roots through `liquid/repository.hpp`, and `liquid gc` uses that repository API.

## What and why

The content store contains acknowledged objects, abandoned interrupted puts, and immutable
chunks shared by many objects. Collection has to reclaim unreachable data without holding the
writer lock through marking or leaving an index entry whose pack frame has already disappeared. It also has to keep
the reference rule beside each object kind so future tree and change records cannot outlive the
objects they name.

A root is a persisted lease under `leases/`. Each record carries a root digest, an expiry, an
owner note, and an internal generation. `store::lease_root` creates it, `lease::renew` writes a
new durable generation, and `lease::release` removes the active generation. Destroying the C++
handle does nothing: expiry, not process lifetime, decides whether the root is live. Renewal
writes the replacement before removing the prior generation, so a crash can extend a lease but
cannot shorten it.

This model has deliberate limits. Leases use the local system clock, so a clock jump can extend
or shorten retention. A holder that stops renewing can lose its root even if its process still
exists, while a very long TTL delays reclamation after a crash. Callers choose the TTL and renew
before it expires. `collection_options::min_lease_age` adds a grace interval after expiry. Remote
lease authority belongs to Phase 6 and is not inferred from local files.

## Epoch rule

Collection snapshots unexpired leases and opens an epoch at `(pack id, frame offset)`. It marks
reachable objects and their chunk-list descriptors, then their chunks. A registry maps an object
kind to a byte extractor returning referenced digests. The built-in `chunk-list` extractor keeps
an object's chunks coupled to the object; the built-in tree extractor retains each entry's object
and reference digest; the change extractor retains its root, parents, conflict references, and
optional symbols. Unknown kinds are safe leaves and are listed in `collection_report::unknown_kinds`.

Each pack gets a bitmap indexed by physical frame position for the epoch. A set bit is live. This
bitmap is working state only: it is never consulted by recovery or index reconstruction. Any frame
at or after the high-water position is implicitly live, including a put that completes while
marking is in progress; the collector asserts that it never adds such a frame to a tombstone set.
Marking releases the store mutation lock and yields to the `io_context` between packs. The final
sweep takes the mutation lock again, includes roots and objects created during marking, and treats
a lease created during the epoch as after the high-water mark for this sweep.

The durable order is:

1. write the filtered object map and the new tombstone bitmap to temporary files, then sync them;
2. rename `gc/MANIFEST` into place as the commit point;
3. rebuild the chunk index without tombstoned frames and durably replace `index/MANIFEST`;
4. delete packs whose frames are all tombstoned, then remove superseded index runs.

Only the committed tombstone manifest is authoritative. A crash before its rename leaves the
previous manifest in force and deletes nothing. A crash after the rename may leave old index runs
or whole packs behind; the next open removes that garbage safely. If `index/` is lost,
reconstruction treats every frame as live unless the committed tombstone bitmap names that frame.
Pack ids are never reused after deletion, so a committed tombstone can never identify a later
frame.

Partially live packs report their dead frame bytes and live fraction as
`bytes_reclaimable_in_partially_dead_packs` and `partially_dead_packs`. After a normal sweep,
liquid #46 may compact those below `collection_options::compact_below`; the exact copy, manifest,
recovery, and reader rules are in [store.md](store.md#pack-compaction).

Repository redaction uses a narrower committed-manifest operation. It removes the selected blob
and unshared content objects from the object map, tombstones their frames immediately, rebuilds
the index, and sends only affected packs through compaction. It then proves the old blob digest
returns `not_found`. This is not an epoch reachability collection and does not disturb unrelated
unleased repository objects. See [`secret-scanning.md`](secret-scanning.md#redaction).

## API and explanations

`collection_options::dry_run` computes the same mark and report without changing objects,
manifests, leases, the index, or packs. `collection_report` returns root, object, chunk, byte,
pack, and epoch counts. Setting `collection_options::explain` also returns each retained chunk
with either its lease path or the epoch high-water rule as the reason.

Repository collection first checkpoints its working tree and leases that tree, every current ref
head, the repository checkpoint document, and lazy directory documents for 30 days. It renews
those leases at every open and checkpoint, and releases superseded checkpoint, ref, and directory
leases while the repository remains open. Call `repository::collect` for a repository; calling
`store::collect` directly on its store can discard live repository objects.

The CLI opens the supplied repository path, or the current directory when omitted:

```sh
liquid gc [--explain] [--compact] [--compact-below <fraction>] [repository-path]
```

`--explain` prints lines such as `blake3:… retained: lease … for root blake3:…` and identifies
chunks protected because they were written after the epoch began.

## How to verify

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build -R GarbageCollection --output-on-failure
LIQUID_SIM_SEEDS=2000 ctest --test-dir build -L sim-long --output-on-failure
```

The focused tests cover two simultaneously leased roots, unrooted reclamation, expiry, renewal,
release, shared chunks, a concurrent put, dry-run immutability, index reconstruction, and 30% and
70% live-pack policy boundaries. The seeded simulator interleaves rooted and orphan puts with
renewal, collection, and compaction-only passes and interrupts writes, renames, and pack removal.
The named seed-140 regression replays a collection or compaction crash and verifies that a forced
index reconstruction retains every root.

## How to change

Add an extractor when an object kind gains its canonical encoder: register the kind in
`src/store/references.cpp`, return every digest whose lifetime is coupled to the record, and add
a test where the referenced object is otherwise unreachable. A pack format change must preserve
stable physical frame positions for the bitmap or version the collection manifest with a
migration. Never reverse the manifest/index/pack-removal order.
