# Store garbage collection

**Status: implemented for single-process stores through the persistence backend (liquid #8, #46,
#58, #278).** Leased roots, epoch
marking, chunk-list reference extraction, whole-pack deletion, sparse-pack compaction, dry runs,
retained-chunk explanations, rule-driven retention, and cost-aware build-result eviction are
available through `liquid/store.hpp`. Repositories retain their own checkpoint roots through
`liquid/repository.hpp`, and `liquid gc` uses that repository API.

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

Lease records are backend objects under `leases/`; they are not assumed to be local files. Renewal
puts the replacement, crosses a durability barrier, and then removes the prior generation.

This model has deliberate limits. Leases use the local system clock, so a clock jump can extend
or shorten retention. A holder that stops renewing can lose its root even if its process still
exists, while a very long TTL delays reclamation after a crash. Callers choose the TTL and renew
before it expires. `collection_options::min_lease_age` adds a grace interval after expiry. Remote
lease authority belongs to Phase 6 and is not inferred from local files.

## Backend byte reclamation

Collection and compaction apply the same ordering to disk and remote backend bytes. The collector
publishes the tombstone manifest, rebuilds and publishes the run set, crosses the required
durability barriers, and only then removes a wholly dead `packs/pack-<id>.pack` object. Compaction
writes and seals the replacement pack, publishes its run set and collection manifest, and removes
the old pack after those objects are durable. A backend failure stops the operation with its liquid
error; it never advances metadata past bytes the backend has not accepted.

The report's reclaimed-byte counts describe physical pack-object bytes removed from the selected
backend. With a future remote implementation this is remote object-store capacity; local cache
eviction and registered remote blob locations remain separate operations. Collection does not
delete content from a `remote_blob_source` registration or from the independent
`blob_write_through` destination because neither is owned by the store backend contract.

## Epoch rule

Collection snapshots unexpired leases and opens an epoch at `(pack id, frame offset)`. It marks
reachable objects and their chunk-list descriptors, then their chunks. A registry maps an object
kind to a byte extractor returning referenced digests. The built-in `chunk-list` extractor keeps
an object's chunks coupled to the object; the built-in tree extractor retains each entry's object
and reference digest; the change extractor retains its root, parents, conflict references, and
optional symbols. Unknown kinds are safe leaves and are listed in `collection_report::unknown_kinds`.
The build cache adds the action and evidence record kinds. An `action-index-root` retains its
verification attestations and reaches each indexed result; a `result` reaches its action,
output/stdout/stderr blobs, provenance, and SBOM attestations; an `sbom/1` attestation reaches its
canonical `sbom` record; and an `action` reaches its input tree, toolchain tree, and sandbox
definition. The index root uses a persisted repository lease, while each result is a separate
retention root. This keeps every usable result graph coupled without allowing the index itself to
defeat result expiry or eviction. Expiry or explicit release makes the complete cached result graph
collectible together.

Each pack gets a bitmap indexed by physical frame position for the epoch. A set bit is live. This
bitmap is working state only: it is never consulted by recovery or index reconstruction. Any frame
at or after the high-water position is implicitly live, including a put that completes while
marking is in progress; the collector asserts that it never adds such a frame to a tombstone set.
Marking releases the store mutation lock and yields to its executor between packs with
`runtime::yield`. The final
sweep takes the mutation lock again, reloads the committed tombstone manifest so that tombstones
added by an eviction or redaction during marking stay dead, includes roots and objects created
during marking, and treats a lease created during the epoch as after the high-water mark for this
sweep. The mutation lock is the store's thread-safe asynchronous mutex; see the mutation mutex
section of [store.md](store.md).

The backend publication order is:

1. put the filtered object map and the new tombstone bitmap, then cross a durability barrier;
2. atomically put `gc/MANIFEST` as the commit point;
3. rebuild the chunk index without tombstoned frames and publish `index/MANIFEST` after its barrier;
4. delete packs whose frames are all tombstoned, then remove superseded index runs.

Only the committed tombstone manifest is authoritative. A crash before its atomic put leaves the
previous manifest in force and deletes nothing. A crash after the put may leave old index runs
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

Repository collection first checkpoints its working tree and leases that tree, every ref selected
by retention policy, the repository checkpoint document, and lazy directory documents. Working
tree, checkpoint, and directory leases use 30 days; ref leases use their matching rule. It renews
the renewable leases at every open and checkpoint and releases superseded roots. Call
`repository::collect` for a repository; calling
`store::collect` directly on its store can discard live repository objects.

Every repository-created lease owner starts with `repository:<repo-id>`. Ref and result retention
updates remove only leases under that repository prefix. This is required when one caller-owned
store holds many repositories: collecting or changing policy for one repository cannot delete the
other repositories' roots, while shared chunks stay live if any repository lease reaches them. The
collector recognizes retention metadata after this prefix, so explanations and per-rule counts keep
reporting the configured rule name.

The CLI opens the supplied repository path, or the current directory when omitted:

```sh
liquid gc [--explain] [--compact] [--compact-below <fraction>] [repository-path]
```

`--explain` prints lines such as `blake3:… retained: lease … for root blake3:…` and identifies
chunks protected because they were written after the epoch began.

## Retention

Repository `config.retention.rules` is an ordered list of three typed rule values:

```text
refs <glob> -> forever | <duration>
results -> <base duration> weighted by cost
size cap -> <bytes>
```

The first matching ref rule wins. A matching ref becomes a named lease root, so the existing
reference registry computes everything reachable from the change; callers never enumerate the
objects themselves. A missing match creates no ref-policy lease. The default policy keeps every ref
and every result for 30 days. `forever` is represented as the maximum store lease expiry and is
handled without expiry arithmetic.

Result leases are anchored to the result's recorded `observed_at`, not extended on every
collection. For the present indexed result set, liquid takes the median positive `timing.exec`
cost and computes:

```text
ttl = base * clamp(max(exec, 1 ms) / median_cost, 1, 64)
```

The store-size cap measures physical pack bytes. When it is exceeded, liquid orders live results by
recorded execution cost per logical result-graph byte, releases enough of the lowest-valued result
leases to cover the excess, and then invokes the ordinary epoch collector. Eviction does not erase
objects, edit tombstones, rebuild the chunk index, or remove packs itself. Only the collector's
manifest commit can make bytes dead, which preserves its crash rule.

`repository::retain_explain` and `liquid retain explain <digest>` run the retention pass and a dry
reachability collection. A retained object names its rule, lease ID, and numeric expiry (or
`forever`); an unretained object says that no live lease reaches it and that the next collection
will evict it. `collection_report::retained_by_rule` counts live policy roots by rule name, and
`retained_objects` carries object-level explanations alongside the existing chunk explanations.
Remote-tier policy remains outside this local implementation.

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
