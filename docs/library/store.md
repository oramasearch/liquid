# Content-defined chunk store

**Status: implemented for single-process stores with an injected persistence backend, local chunks,
registered remote blobs, pre-pack write-through, local eviction, and resemblance-selected chunk
deltas (liquid #1, #7, #8, #17, #46, #116, #133, #199, #208, #278), plus opt-in exclusive
cross-process ownership with a bounded hand-off between processes (liquid #321), repository
state routed through the backend with a conditional create (liquid #326), an injected owner
whose grant fences stale writers on backends without a local root (liquid #327), and a resumable,
verifiable whole-repository transfer between two open stores (liquid #328). No backend write
rewrites bytes already written: every object is appended in order or replaced whole (liquid #332).**
`store::open`, `put`, `put_file`, `get`, `get_to_file`, `read`, and `contains` are Capy
coroutines. The same operations support encrypted objects when supplied a keyring. Leased roots
and epoch collection are implemented as Capy coroutines; see [gc.md](gc.md). Collection also
compacts sparse packs without recompressing frames. The Phase 1 large-input exit criterion is met.

## What and why

The store accepts bytes and returns a BLAKE3 digest for the complete input. It splits content
with FastCDC, stores raw chunks once, and records the input's ordered chunk list. Reading verifies
each chunk's digest after decompression. The layout lets source, build output, and large binaries
share chunks without a file-per-object bottleneck.

## Store backend interface

`store_options::backend` selects the persistence implementation. A null value selects the disk
reference implementation rooted at the path passed to `store::open`; callers can also construct it
with `make_disk_store_backend`. The public `store_backend` contract has only the operations the
store needs:

- `put(key, bytes)` atomically replaces one complete object;
- `put_if_absent(key, bytes)` stores one complete object only when the key is absent, and fails
  `errc::already_exists` otherwise, as one atomic decision (liquid #326);
- `put_range(key, offset, bytes)` creates or extends a pack-like object at an exact byte offset,
  which the store always makes the object's current end. The store never rewrites bytes already
  written (liquid #332), so a backend may refuse any other range with `errc::conflict`, as Zetta's
  object storage does;
- `get_range(key, offset, size)` returns exactly that byte range;
- `remove(key)` deletes one complete object;
- `list(prefix)` returns matching keys and sizes; and
- `barrier()` completes the backend's configured durability level for all successful operations
  before later publication; and
- `fenced(fence)` returns a view of the same objects whose every call carries an injected owner's
  fence. It is not pure virtual: the default is `errc::unsupported`, which only an open with
  `store_options::owner` reaches (see [Injected owners and fencing](#injected-owners-and-fencing)).

Keys use `/` as a logical separator and are relative to the backend root. `LIQUID_STORE`,
`packs/`, `index/`, `leases/`, `gc/`, and a shared store's `repositories/` are backend keys, not
paths exposed to store code. A
complete-object `put` must be atomic for readers. A range outside an existing object's bytes is
`errc::corrupt`; a missing key is `errc::not_found`. Network implementations map an unreachable
service to `errc::unavailable` and rejected credentials or permissions to `errc::unauthorized`.
Malformed or truncated stored bytes are `errc::corrupt`. The disk implementation additionally
uses `errc::io` for local operating-system failures. Every error carries the logical key or local
path in `error::context`.

The disk implementation retains mapped index runs and Corosio range I/O. The in-memory
implementation under `tests/sim/` stores only byte vectors, supports coroutine latency and a
one-shot injected `error`, and survives store reopen while its shared backend handle remains alive.
The same seeded crash schedule runs against both implementations.

`tests/sim/write_once_backend.hpp` decorates either one with the range contract of Zetta's object
storage (zetta#1005, liquid #332): `put_range` appends only at the object's current end, a retry that
resends bytes already held succeeds without a change, and any other range, a rewrite or a gap, fails
`errc::conflict`. Its `fenced()` decorates the decorated backend's fenced view the same way, so a
store held by an injected owner keeps the contract. The crash schedule runs over it on both
implementations, and CTest runs the index suite (`index-write-once`) and the store, collection and
repository transfer suites (`store-write-once`) a second time over it, selected by
`LIQUID_TEST_WRITE_ONCE_BACKEND=1`: disk stores sit behind it, and so does the transfer's
remote-shaped destination, because lake transfers into Zetta. It hides the disk root, so those runs
load index runs into memory as a remote store does.

## Layout and pack format

```
<root>/LIQUID_STORE       format marker: `liquid-store 1\n`
<root>/packs/pack-<id>.pack
<root>/packs/objects.meta ordered input digest → chunk-list descriptors
<root>/packs/remotes.meta remote whole-content digest → size, chunks, source, instance
<root>/index/MANIFEST
<root>/index/run-<seq>.idx
<root>/leases/lease-<id>-<generation>.lease
<root>/gc/MANIFEST       committed epoch and per-pack tombstone bitmaps
<root>/keys/<key-id>.wrapped   encrypted trust-domain data key
<root>/repositories/<id-hash>/LIQUID_REPO     shared-store repository checkpoint entry point
<root>/repositories/<id-hash>/LIQUID_JOURNAL  shared-store repository journal entry point
<root>/repositories/<id-hash>/replication/<source>.cursor   replication peer resume position
<root>/repositories/<id-hash>/sessions/<session-id>/…      private session state (same names)
<root>/tmp/
<root>/store.lock          exclusive owner lock and the owner's pid, host and start time
```

The disk backend's `open` creates missing directories and refuses a marker other than format 1 with
`errc::unsupported`. A pack begins with its id. Every following chunk is one append-only frame:
an eight-byte frame magic, raw and compressed lengths (`u32` each), an algorithm byte, the 32-byte
digest, and an individually zstd-compressed payload. The raw length sizes the decompression buffer
only when it is at most 16 MiB and, when the zstd frame records its content size, equal to it;
anything else is `errc::corrupt` (L-27, liquid#411). A delta frame uses its own magic and prefixes
the payload with the base algorithm and 32-byte digest; the remaining bytes are zstd-compressed
with the complete base chunk as a dictionary. A sealed pack ends with a sorted chunk table and a
footer containing the table location and checksum. The table carries the algorithm tag, so it
remains the source for rebuilding index run sets. The frame header carries enough information to
recover a delta reference without changing the fixed-width table or public chunk interface.

Ordinary writes use the store's configured hash algorithm. Verified sync receipt instead preserves
the algorithm tag carried by each frame after recomputing that exact digest, so a BLAKE3 and a
SHA-256 object can coexist in either configured store without changing remote identity.

The `repositories/` directory appears only when a caller supplies one open store to
`repository::create` or `repository::open`; it is not part of a repository-local store's checkout
layout. Repository documents, directory checkpoints, trees, blobs, and chunks are still stored
through the ordinary content-addressed packs, so identical content has one physical frame across
all repositories in the store. `<id-hash>` is the hex BLAKE3 digest of the identifier passed to
`create` or `open`.

## Repository state through the backend

**Why (liquid #326, lake#512).** Lake keeps each workspace's store in the workspace
Zetta, through a remote `store_backend` over Zetta's object capability, and cannot mount the
workspace's volume. Before this change a repository's own entry points were read, written and
renamed with `std::filesystem` beside the store, so a store without a local root could hold the
packs but not the repository. Every piece of repository state is now a backend key.

**Keys.** A repository opened on a shared store keeps its state under `repositories/<id-hash>/` in
that store's own backend:

| Key | Contents | Written with |
|---|---|---|
| `…/LIQUID_REPO` | `<document digest>\n`, the checkpointed repository document | `put`; `put_if_absent` on create |
| `…/LIQUID_JOURNAL` | `liquid-journal-v2 <document digest>\n`, the newest uncheckpointed document | `put`; `remove` at checkpoint |
| `…/replication/<source>.cursor` | the sync cursor token pulled from peer `<source>` | `put` |
| `…/sessions/<session-id>/LIQUID_REPO`, `LIQUID_JOURNAL`, `LIQUID_SESSION` | a private session replica's entry points and authority | `put`; the prefix is removed on publish or discard |

A repository with its own store keeps the same names beside its working tree, through a disk
backend rooted at the checkout: `<checkout>/LIQUID_REPO`, `<checkout>/LIQUID_JOURNAL`,
`<checkout>/sessions/…`, and `<checkout>/.liquid/replication/<source>.cursor` (`.liquid/` is the
checkout's private directory, so a cursor is never captured as a working-tree file). The
checkout's state files are written with a data sync, as before.

`repository::replication_cursor(source)` and `set_replication_cursor(source, cursor)` read and
publish a cursor. `source` must be `[A-Za-z0-9._-]`, not beginning with a dot. The cursor is local
state: it is not part of the repository document and is never exported to peers.

**Ordering.** Every state publication runs `barrier()` on the store's backend, so the pack frames
and the document the marker names are complete before the marker; then the atomic `put` (or
`put_if_absent`); then `barrier()` on the state backend, so a later publication cannot overtake
it. This is the order pack data and `index/MANIFEST` already use. The journal is a single pointer
record replaced atomically, not an append log: each journaled write stores its whole document as
an ordinary object and replaces the pointer, so a reader sees the previous pointer or the new one
and never a torn record. A crash before the replacement leaves the previous journal and loses only
the unacknowledged write. Open still reads a legacy journal holding an inline document.

**Exclusive create.** `repository::create` first refuses an existing `LIQUID_REPO` cheaply, before
writing any object. The create's first checkpoint then publishes `LIQUID_REPO` with
`put_if_absent`, so of two concurrent creators of one identifier exactly one succeeds and the other
fails `errc::already_exists` with no marker of its own; the objects it wrote are unreferenced and
are reclaimed by collection. The disk backend writes the bytes to a temporary name unique to the
call (`<key>.<random>.backend-tmp`, which `list` skips) and hard-links it to the key: `link(2)`
refuses an existing destination atomically. The in-memory backend decides under its lock. The base
class refuses with `errc::unsupported`, so a backend that has not implemented the conditional create
cannot host repository creation rather than emulating it with a racy test-then-put. Open of an
identifier with no `LIQUID_REPO` is `errc::not_found`.

**Disk list.** `list(prefix)` on the disk backend walks only the prefix's directory and the entries
whose names start with its last component, so a backend rooted at a checkout answers for
`LIQUID_REPO` without visiting the working tree. The keys returned are unchanged.

**What still uses a local path.** The structural-merge parse graph is a cache, not repository state.
It stays at `<state>/STRUCTURAL_MERGE_GRAPH` when the state backend has a local root; otherwise
`import_drips` opens it in a process temporary directory removed when the merge ends. Splits create
an owned checkout repository by design and remain filesystem operations.

**Compatibility.** The on-disk layout and the marker and journal formats are unchanged, so no
format marker changes and there is nothing to refuse: a repository written before this change opens
after it, and one written after it opens with an earlier build. A `LIQUID_REPO.tmp` left by an
earlier release's interrupted rename is ignored. Disk replacement now goes through the backend's
`.backend-tmp` temporary and rename.

**Verify.** `tests/sim/test_repository_state.cpp`: the conditional create on both backends,
including eight racing creators of one key; the default `unsupported`; the scoped disk `list`; a
shared-store repository on the in-memory backend that is created, written, cut, synced to a second
repository, given a replication cursor, reopened, and collected with no file under any directory
and every backend key under the store's known prefixes; the unchanged disk layout and an
earlier-release temporary; a crash before and after the journal publication on both backends; and
the repository crash schedule described in [simulation.md](simulation.md#repository-state).

The current pack has no table or footer. Its frames are represented in memory and in the index
memtable. When a pack reaches `store_options::pack_size` (or 64 MiB when it is zero), it is sealed:
the table and footer are appended once and the next chunk starts a new pack. No pack is truncated or
rewritten.

On open, a valid footer selects the table as before. A pack without a valid footer is an unsealed
pack: the store scans frames from the beginning, decompresses and hashes each one, and stops at the
first invalid or partial frame. Earlier valid frames are recovered; the trailing bytes are left
unchanged and a later write uses a new pack. This makes an interrupted append recoverable without
making a partial frame visible.

## Encrypted objects

When `store_options::keys` is set, `put` selects `store_options::write_key` or the keyring's
default key id. That id names a trust domain. The keyring unwraps its domain data key from
`keys/<key-id>.wrapped`; BLAKE3 derive-key creates distinct keys for chunk identifiers and
XChaCha20-Poly1305 encryption. Chunk ids are `blake3k` keyed digests, so a storage server cannot
test a guessed plaintext. The data key is random when the domain is first written and the KEK
never directly identifies or encrypts a chunk.

Encrypted frames use the encrypted variant of the frame magic, and their compressed payload is
replaced by `nonce[24] || tag[16] || ciphertext`. The nonce comes from the operating system random
source for every new chunk. Recovery can structurally recognize a complete encrypted frame; its
AEAD authentication and keyed digest are verified when the chunk is read with its trust-domain
key. The object map format is `LQOBJ4`: each record adds raw chunk sizes to `LQOBJ3`'s descriptor
digest and object kind, and `LQOBJ2`'s encryption flag and optional length-prefixed key id. The
sizes let `read` jump to
the first chunk overlapping a large offset without loading preceding chunks. `LQOBJ5` remains
readable, but resemblance values are write-path-only and new catalogs use `LQOBJ4`. A zero
descriptor algorithm is the sentinel for legacy records that predate stored chunk-list objects.
Its chunk-list descriptor contains the same key id and is encrypted too. `LQOBJ1` plaintext and `LQOBJ2`
encrypted metadata remain readable, as do `LQOBJ3` and `LQOBJ4`; the next metadata write fills
legacy chunk sizes from pack tables before publishing `LQOBJ4`.

Every encrypted read first unwraps the domain data key, then authenticates and decompresses each
requested chunk and verifies its keyed id. A missing or wrong key is `unauthorized` with the key
id in `error::context`; ciphertext authentication failure or keyed-id mismatch is `corrupt`.
`get` never returns ciphertext as content.

## Chunking and integrity

FastCDC uses a deterministic xorshift-derived 256-entry gear table seeded with
`0x9e3779b97f4a7c15`. It cuts at 8 KiB minimum, 64 KiB target, and 128 KiB maximum. The algorithm
is platform-independent. `put` produces BLAKE3 identities by default or SHA-256 identities when
`store_options::hash_algorithm` selects it. The index and pack table retain the algorithm for every
entry, so one store can resolve both forms. Every `get` and `read` rehashes raw chunk bytes; a
mismatch returns `errc::corrupt` with the digest in `error::context`. See
[hash agility](hash-agility.md) for the supported algorithms, rehashing behavior, and signature
scheme tags.

### Near-duplicate chunk compression

Plaintext chunks also receive a 64-bit resemblance hash: the minimum mixed value of four evenly
spaced 32-byte buzhash samples. This scans at most 128 bytes once per chunk, rather than making
the write path proportional to chunk size. The value is computed once and carried through the
chunk write and object metadata paths. Importers that know a source path pass it as the candidate
scope; unscoped store writes keep no candidates, preserving their independent retention lifecycle.
Only chunks of at least FastCDC's 8 KiB minimum may enter the delta candidate cache. The cache
keeps one candidate for each path scope, at most 256 candidates and 16 MiB of candidate
bytes across all scopes. The open store permits dictionary attempts for at most 256 path scopes;
the first matching candidate for each permitted scope uses zstd level 1 through
`runtime::offload`, and later matches retain ordinary compression. Raw bytes are retained only
when a scope has no candidate. It does not read, decode, or
re-hash a base on the write path. Chunks without a matching candidate retain the established direct
zstd level-3 compression path. It writes a delta only when the result, including the 33-byte base
identity, is at least 64 bytes smaller than half the raw chunk. Encrypted chunks do not participate
because cross-object plaintext comparison would cross the trust-domain boundary. Delta chains are
bounded at eight links.

The design choice was measured over all 950 unique versions of
`single_include/nlohmann/json.hpp` in the vendored nlohmann/json history at commit
`8167d2f31858884a07f4c0e700e5b1d937051ddb`. Each row includes the 49-byte frame header and the
53-byte sealed-pack table entry; payloads use zstd level 3.

| Candidate | Unique raw chunk bytes | Compressed bytes including frame/table overhead |
|---|---:|---:|
| Existing 64 KiB FastCDC | 244,535,197 | 45,146,899 |
| 16 KiB FastCDC for the changed path | 97,336,663 | 20,524,744 |
| Previous version, matching path and chunk ordinal | 244,535,197 | 4,417,423 |
| Previous file prefix as one zstd dictionary | 244,535,197 | 38,487,049 |
| Rolling-shingle resemblance selection | 244,535,197 | 9,191,259 |

The path-and-ordinal result is a useful lower bound but requires the repository importer to expose
versioned path identity to the content store. The smaller-chunk option more than doubles frame and
index entries for this history and remains over twice the size of resemblance selection. The
resemblance hash was chosen because it remains behind the existing chunk interface and approaches
the path-aware lower bound without persisting repository paths.

Readers resolve a base digest recursively through the same pack index, decompress the target with
that verified base, and verify the reconstructed target digest. The base is therefore an ordinary
content-addressed object, not a physical pack offset. Sealed-pack tables, index runs, range reads,
and callers retain their existing shapes.

`read` loads only the chunk headers and compressed byte ranges that overlap the caller's range.
Disk-backend pack reads and writes use Corosio `random_access_file`; synchronous directory creation
and atomic complete-object renames are limited to filesystem metadata operations and use
`runtime::offload`. Corosio open failures are converted to `errc::io` at the private boundary.

## Remote blob availability

`store::register_remote` records a blob's whole-content digest, size, chunk digests, chunk sizes,
source id, and instance in `remotes.meta`. `store::has` distinguishes `absent`, `remote`, and
`local`; a registered blob becomes local only when every named chunk is present. The configured
`store_options::remote_sources` map resolves source ids to `remote_blob_source` handles.
`store::register_remote_source` adds or replaces a source after the store is open so long-running
node processes can discover repositories without reopening their shared store.

`get` fetches all missing chunks and verifies the whole-content digest. `read` fetches only complete
chunks intersecting its requested range. Every returned range is length-checked, each chunk is
hashed before `put_plain`, and the assembled object is hashed before `get` succeeds. Remote bytes
therefore enter packs through the same verified, deduplicated path as local writes. The separate
metadata file is atomically replaced and survives reopen; it carries locations, not content.
`store_options::on_hydration` observes each newly fetched chunk after it has entered the verified
store path; the completion flag identifies the last chunk in the blob's ordered table. Node-store
uses the coroutine callback for per-image counters and budget checks.
`store_options::on_remote_access` observes successful reads of remote-registered content, including
reads served entirely from local chunks; node-store uses it for least-recently-read ordering.
`store::local_bytes` reports the deduplicated raw size of live indexed chunks, excluding obsolete
and evicted pack entries.

## Write-through ordering

`store_options::write_through` is an optional shared `blob_write_through`. For a new whole-content
identity, the store computes the whole digest and the complete ordered chunk digest/size table,
then awaits exactly one `blob_write_through::put(content, bytes, chunks, chunk_sizes)` call before
the first chunk or descriptor reaches a pack. A successful callback permits the ordinary local
write. An error is returned unchanged; no pack frame, index run, index manifest, object map, or
collection manifest is modified, and a previously unknown digest remains `absent`. A duplicate
whole-content identity that is already present returns without another callback.

The callback and the persistence backend are separate contracts. After a successful callback, pack
frames and the chunk-list descriptor are written through the selected backend. A durability barrier
completes before the object map can publish the object, and run-set and collection manifests use the
same data-before-metadata ordering. A successful `store::put` therefore means the selected backend
has accepted every byte needed to recover the object at its configured durability level. The disk
backend preserves the existing `store_options::durable` per-range synchronization behavior while
publication barriers order metadata in both durability modes. With `durable == false`, a process
crash remains recoverable but power-loss guarantees still depend on the operating system cache.

An unreachable write-through target returns `errc::unavailable` before the local disk store is
changed. Existing local objects, registered-local chunks, and repository state remain readable
offline; hydration fails only when the requested bytes are absent locally. A store explicitly
configured with an unreachable remote persistence backend also returns `errc::unavailable` and has
no implicit disk fallback. Offline operation uses the disk backend, with remote publication kept in
the optional write-through callback.

The callback is a lazy Capy coroutine running on the caller's runtime executor. It performs its own
I/O with Corosio or `runtime::offload`; it must not block the executor or call `runtime::block_on`.
The byte and table spans remain valid until the returned task completes. The `chunks` and
`chunk_sizes` spans have equal length, sizes sum to `bytes.size()`, and concatenating those logical
chunks yields `bytes`. Plain stores report the selected BLAKE3 or SHA-256 identities. Encrypted
stores report keyed chunk and whole-content identities while `bytes` remains plaintext.

For `store::put`, `bytes` spans the caller-supplied in-memory content even when FastCDC produces
many chunks. For `store::put_file`, the store first computes the digest and chunk table with its
bounded 8 MiB streaming scan. It then gives the callback one read-only, file-backed mapping of the
complete source; the mapping is not a heap copy and is valid only for that callback. The store
verifies the mapped chunks against the streaming scan before invoking the callback and copies at
most one chunk into the pack path at a time after success. An empty file is represented by an
empty byte span and one zero-length chunk. Thus large files retain bounded heap use while the
single callback can consume the content sequentially from `bytes`.

## Evicting a local remote copy

`store::evict(content)` requires a persisted registration for `content` in `remotes.meta`; an
unregistered digest returns `errc::not_remote` and changes nothing. Eviction removes the local
object entry and tombstones locally cached content chunks and the local chunk-list descriptor when
they are not shared by another local object or registered remote blob. It publishes the object
map, then the collection tombstones, then the rebuilt chunk index, without changing the remote
registration. The map goes first, so a crash between them leaves no object naming a tombstoned
chunk ([gc.md](gc.md), liquid #337). Evicting an already remote-only registration succeeds without a write.

When no retained object or registration shares every chunk, `has(content)` reports `remote`, and
`get` or `read` hydrates verified bytes again through the registered `remote_blob_source`; range
reads still fetch only intersecting chunks. Eviction does not rewrite a pack. Whole dead packs and
space in partially dead packs are reclaimed by the existing collection and compaction paths.

`put` acknowledges a chunk after its complete frame has been written. `store_options::durable`
defaults to `false`; setting it to `true` calls `sync_data` on the pack before acknowledgement.
The ordering makes a process crash safe: complete frames can be found by the recovery scan, and torn
frames are ignored. A power loss needs `durable` for the operating system to make that guarantee.
An interrupted put may remain as an orphan: **acknowledged ⊆ visible ⊆ correct**. A visible object
is always verified against its digest, and collection reclaims unleased orphans.

## Streaming files

`store::put_file` opens its source with Corosio `random_access_file` and reads 8 MiB windows. A
stateful FastCDC chunker carries only the current chunk across window boundaries, so it produces
the same ordered cuts as `put` without retaining the input. BLAKE3, keyed BLAKE3, or SHA-256 is
updated for each window and finalized only after the last byte. Without a callback, each completed
chunk is deduplicated and framed immediately. With a callback, the scan writes no frame; the
verified file mapping is followed by chunk-at-a-time framing only after the callback succeeds. The
whole-object chunk-list descriptor and `LQOBJ4` metadata are published at the end.

`store::get_to_file` loads and verifies one chunk at a time and writes it with Corosio
`random_access_file`. It incrementally verifies the whole-object digest before acknowledging the
export. A one-chunk cache avoids reopening and decompressing consecutive duplicate chunks while
remaining bounded. `repository::write_file` imports through `put_file`, derives its canonical blob
record from that stored ordered chunk list, and records one drip. The in-memory repository write
uses the same stored list, so both paths produce the same blob record. Raw blob chunks remain
readable under encryption through their parent object's trust-domain key.

## Delta records and remote fetch

`delta_record` is a 108-byte versioned record: eight-byte `LQDLTA` version magic, source digest,
target digest, one-byte algorithm id, and patch-blob digest. The reference registry classifies it as
`delta` and traverses all three digests. A `delta/1` attestation traverses to its record. The public
`store::attestations(subject)` view exposes the rebuilt subject index used by delta discovery and
attestation policy without exposing pack or index types.

The on-disk `remote_blob_source` test double from #17 supplies registered patch and target blobs.
Its range reads retain the store's per-chunk and whole-object verification. `fetch_delta` uses a
verified catalog match when available; otherwise it reads the target through ordinary local or
registered remote chunk metadata. Content-defined chunks remain the baseline delivery mechanism,
and the reconstructed destination is always checked against the requested target digest.

The default suite exercises this path with a deterministic sparse 256 MiB file. The labelled
`large-40g` test uses a 40 GiB file and a 40-byte file through the same `write_file` call. It
verifies three 4 KiB ranges through `store::read`, verifies the middle range through VFS hydration,
exports the large object, re-imports the export, compares digests, and asserts peak RSS below 1
GiB.

The recorded run used the default sparse fixture: zeros throughout with three seeded 4 KiB marker
blocks at byte 0, 20 GiB, and 40 GiB minus 4 KiB. The store deduplicated that input to about 15 MB.
Consequently, its measurements prove bounded memory and the streaming code path over a 40 GiB
input; they do not measure unique-data throughput.

Set `LIQUID_40G_RANDOM=1` to generate the source as a streamed seeded pseudo-random byte sequence
instead. That mode creates about 40 GiB of packs and is opt-in because it needs the corresponding
disk capacity. For example:

```sh
LIQUID_40G_RANDOM=1 cmake -S . -B build -G Ninja -DLIQUID_LARGE_TESTS=ON && cmake --build build && ctest --test-dir build -L large-40g --output-on-failure
```

Recorded on an Apple M3 Max with 64 GiB RAM and macOS 14.6.1:

| Measurement | Result |
|---|---:|
| `repository::write_file` wall time | 177.189 s |
| Import throughput | 231.165 MiB/s |
| Full labelled-test wall time | 587.936 s |
| Peak RSS (`getrusage`) | 400,048,128 bytes (381.5 MiB) |
| Store on-disk logical size | 15,033,445 bytes (14.3 MiB) |

The source and exported temporary files were deleted after the run. The small store size is
expected for the sparse fixture: repeated zero chunks deduplicate and compress, while the full
40 GiB logical byte stream still passes through hashing, FastCDC, range reads, export, and digest
verification.

## Garbage collection

Collection snapshots unexpired leased roots and marks objects and chunks through a per-kind
reference registry. The mark bitmap is working state for that epoch only; the committed manifest
records tombstones for dead physical frames. Frames written after the epoch high-water position
are live without joining the snapshot and cannot become tombstones. The index is rebuilt without
dead chunks before wholly dead packs are removed. Sparse packs below the collection threshold are
compacted as described below. The full lease model, crash order, report fields, and verification
are in [gc.md](gc.md).

### The collector's yield and the mutation mutex

Two defects met in `GarbageCollection.PutAfterEpochHighWaterSurvives`; liquid #208 fixes both.

The crash. The collector gave up its executor between packs with `runtime::delay(1ns)`. Capy's
`delay_awaitable::await_suspend` arms the timer first and registers its `std::stop_callback`
afterwards. A one-nanosecond timer fires inside that window: the timer thread posts the
continuation, another runner resumes the coroutine, `await_resume` sees no active stop callback
and skips it, the awaiter is destroyed with its frame slot, and the suspending thread then
constructs and registers a `std::stop_callback` in memory the frame no longer owns. That node stays
in the stop state's intrusive list. The collector's next awaiter, the sweep's mutex waiter, reuses
the memory, and when it registers its own callback on the same token the list splice writes the
node's address over the waiter's executor reference; the put's `unlock` then posts through the
corrupted vtable (`pc = 0x0` in `executor_ref::post`, caught with gdb in `put_selected`'s
`lock_guard` destructor; a core shows `executor_ref::vt_` equal to the address of the waiter's own
callback storage). Release 29 of 50 runs on `20e5407` in the `liquid-linux-ci` container; the
write-through change altered the timing enough to make it reproduce on most runs. The collector
now yields with `runtime::yield`, which posts the continuation to the caller's executor and arms
nothing (see [runtime.md](runtime.md)); `liquid verify`'s scheduled graph validation used the same
one-nanosecond delay and was changed too. The ordering in Capy's `delay.hpp` is a Capy defect to
report upstream.

The mutex. ThreadSanitizer on the same tree also reports a real data race in Capy's
`async_mutex`: `collector::run`'s inlined `lock_awaiter::await_suspend` writing the waiter queue
on one `block_on` runner thread against `async_mutex::unlock`'s `pop_front` reading it on another,
inside the backend's `mutation_` member. `async_mutex` is documented for one executor only, while
`runtime::block_on` and the default context each run one `io_context` on several threads, and
Corosio resumes a coroutine after file I/O directly on the completing runner
(`posix_random_access_file::raf_op::operator()` calls `coro.resume()` without dispatching through
the awaiter's executor), so a store coroutine that holds the lock across a pack write comes back on
an arbitrary thread and a Capy `strand` cannot keep the lock on one executor. The backend therefore
serializes mutation with liquid's own `mutation_mutex` (`src/store/mutation_mutex.hpp`): an
asynchronous mutex safe from any thread that follows Capy's IoAwaitable protocol (`io_env`,
`continuation`, `executor_ref`, stop tokens), so call sites read exactly as with
`async_mutex::scoped_lock`. Waiters queue in FIFO order under a short native mutex, resume through
the executor they suspended on, become visible only after every field another thread may read is
written, and are not touched after they are published; an atomic state resolves the race between
unlock and cancellation.

Two strand designs were measured before the cause was found and are recorded because they looked
like fixes: one strand per store over the shared default context, and one per store in the
caller's context. Both replaced the collector's environment through `capy::run` and so masked the
corruption in this test; the first crashed `Sync.DivergentReferenceUpdateIsNotFastForward` with a
bus error when the caller's `io_context` shut down with a cross-context post still queued, and the
second reproduced the original crash in `Store.EvictionDuringCollectionStaysRemote`. Neither is in
the tree.

The ordering invariant the collector relies on is: a put holds the mutation lock from before its
write-through callback until its object is published, and appends no pack frame before that
callback succeeds; collection records its pack and offset high-water position under the same lock,
then treats every later frame as live when it reacquires the lock for sweep. Eviction and
redaction load and advance the committed tombstone epoch while holding that lock, and the sweep
reloads the latest committed manifest under the lock, so a tombstone committed while it marked
stays dead in the rebuilt index even when the frame is after the high-water mark or reachable from
the snapshot. Thus a put is either inside the collection snapshot or retained after its high-water
mark, and an eviction is ordered wholly before the sweep commit or wholly after it and survives
either way.

## Pack compaction

`collection_options::compact_below` defaults to `0.5`. A partially dead pack is compacted when
its live frame-byte fraction is strictly below that value; `0` disables compaction.
`collection_options::compact_only` uses the committed tombstone manifest without another mark and
sweep. Repository collection exposes that pass as `liquid gc --compact`; `--compact-below
<fraction>` changes the threshold for either collection mode.

For each selected pack, the collector holds its mutation lock, records a pending compaction
marker, then copies each non-tombstoned physical frame into a new sealed pack. A copied frame is
its original header and compressed (or encrypted) payload byte-for-byte: compaction never
decompresses, recompresses, or recalculates its identity. The new locations are inserted into a
newest index run. The replacement pack and run are only preparation; the atomic
collection-manifest put is the commit point. That manifest replaces the old pack's partial bitmap
with an all-dead bitmap. Only after that put does the index manifest publish the new run and does the collector
remove the old pack and its pending marker.

Recovery reads a pending marker before opening the index. Before the collection manifest put,
it removes the replacement pack; after it, it removes the old pack. A crash between the collection
manifest and index-manifest updates rebuilds the index from packs and the committed tombstones.
Thus an uncommitted replacement never survives the next open and a committed replacement is found
even if index publication did not finish.

Readers do not take the mutation lock. A reader that resolved an old location before the switch
can still read the old pack until its later removal. On POSIX, a reader that already opened the
file keeps a valid mapping after unlink. On Windows, a removal blocked by an open mapping is left
for the next store open through the pending marker.

`liquid repack [repository]` runs a compaction-only pass over every pack containing a delta frame.
Each delta is decoded and written against the oldest reachable base in its current chain; when that
base no longer saves at least 64 bytes the target becomes an ordinary frame. Other frames are copied
byte-for-byte. The collection report counts rewritten packs, bytes, and input deltas processed.
Repack uses the same pending marker and manifest commit point as sparse-pack compaction, so a crash
leaves either the old pack or the completely sealed replacement authoritative. It never changes a
chunk digest or object descriptor.

## Index

The index is liquid-owned. Its portable records are exactly 48 bytes, serialized as digest bytes
(32), pack id (u32), offset (u64), and compressed length (u32). The digest comes first, so records
sort lexicographically by digest bytes.

An algorithm owns an `std::unordered_map` memtable and immutable run set. The default memtable
limit is 1,048,576 entries; inserts and memtable lookups are O(1), and the table is flushed at
that limit or when its pack seals. The run header carries the algorithm, record count, a
two-byte (65,536-bucket) fanout table of u32 cumulative counts, a 10-bit-per-key bloom filter,
and a checksum.

`MANIFEST` lists live runs with their algorithm and the highest indexed pack id. On the disk
backend, `open` maps each immutable run read-only through the private cross-platform mapping helper;
other backends load the run into owned memory. Lookups check the algorithm's memtable, then runs
newest first. A bloom hit selects
the two-byte fanout slice, and `std::lower_bound` searches the mapped 48-byte records directly.
The index therefore issues no file I/O on its lookup path. Mapping keeps per-run process memory
bounded by the 256 KiB fanout header plus the bloom filter; mapped record pages remain managed by
the operating system. Flush and compaction build that table in a heap `std::vector`: as a coroutine
local array it made their Debug ramps reserve 256 KiB and 516 KiB of the calling thread's stack,
which overflowed a 512 KiB secondary thread (liquid #323).

Runs are size-tiered by record count. When more than four runs share a tier, compaction performs a
streaming k-way merge over the mapped input records. It does not load all input runs into memory.

**Every run is written once, in order (liquid #332).** The header carries the record count and
fanout and the bloom follows it, but a merge learns the count, fanout and bloom only after it has
seen every record, because duplicate digests collapse. Compaction used to write a placeholder
header and an empty bloom, append the records, and then rewrite the header at offset 0 and the bloom
in place. Zetta's object storage, under lake's per-workspace stores,
refuses that rewrite with `409 conflict`. Compaction now merges twice over the immutable inputs. The
first merge writes nothing: it counts the records and fills the fanout and bloom. The second
writes the finished run as a complete-object `put` of the header, then appends the bloom and the
records in 4 MiB windows, each at the object's current end. Flush already wrote this way, and its
bloom is now appended in the same windows. The first merge has no suspension point, so no
concurrent flush lands between the two; the second checks that it produced the counted records.

The two merges were chosen over the alternatives the issue named:

- **Records first into a staged object, then a separate finished object.** This needs no format
  change, but it writes every record twice and reads it back once, and it leaves a second
  unpublished object to clean up after a crash.
- **A header-last layout** (records, then bloom, then a footer carrying count, fanout and
  checksum). This writes every byte once but changes the run format. Every existing run would need
  a second reader, and `open` would need the footer's position before it could map the fanout.

A second merge reads mapped input pages again and costs CPU, but adds no write and no format.
Memory stays at the fanout table, the bloom and one 4 MiB window, as before, and all three stay on
the heap (liquid #323).

**Crash safety is unchanged.** The replacement run is published only by the manifest put that
follows its durability barrier, so a merge torn at any write leaves the manifest naming the old
runs. The next merge takes the same unpublished key, because `next_sequence_` comes from the
manifest, and its complete-object `put` replaces the torn object whole rather than rewriting
its bytes. The old runs are unmapped and removed only after the manifest names the replacement.

If the manifest or a run header is absent or corrupt, `open` reconstructs the run sets from pack
tables. Deleting `index/` is therefore recoverable. There is no index write-ahead log.

Concurrent coroutines on one store serialize mutation with the store's thread-safe
`mutation_mutex`. Reads use immutable
runs and are not given that mutation lock. Across processes, see the next section.

## Process ownership and hand-off

**What.** One open store handle owns a disk store root at a time. An open with
`store_options::exclusive_owner` takes an exclusive advisory lock on `<root>/store.lock` (`flock`
on Linux and macOS, `LockFileEx` on Windows) before it reads or repairs anything under the root, and
holds it until the `store` is destroyed. It then writes its pid, host and start time into the file.
A second exclusive opener, in this process or another, retries until `store_options::owner_wait`
has elapsed and then fails with `errc::locked`, whose message names the holder:

```text
store root is owned by pid 4211 on host lake-7d9f since 2026-10-02T21:14:03Z; one open store
handle owns a root at a time
```

A zero wait makes one attempt. A negative wait, or a wait without `exclusive_owner` or an injected
`owner`, is `errc::invalid_argument`; `exclusive_owner` with a persistence backend that has no local
root and no injected `owner` is `errc::unsupported`. The descriptor is close-on-exec, so a child process never keeps the root.
The kernel releases the lock when the owner's descriptor closes, however the process ends, so a
successor never waits on a dead owner.

Opens without `exclusive_owner` take no lock and keep the single-process contract: the caller must
ensure that no other process writes the root. Existing callers that open one repository's store
from several handles in one process (repository reopen, the CLI beside its daemon, readers in
tests) are unchanged.

**Why (liquid #321, found by lake#467).** A store keeps its pack append position, object map,
chunk index memtable, and delta candidates in process memory. It appends at the offset it
remembers, publishes `packs/objects.meta` and `index/MANIFEST` by replacing them from that memory,
and on open removes the fixed-name `gc/MANIFEST.tmp`, `packs/objects.meta.tmp`, and
`packs/remotes.meta.tmp` that a live writer may be about to publish. Two processes writing one
root therefore overwrite each other's frames and object maps; lake measured `chunk header digest
differs from index`, `invalid index run size`, lost acknowledged writes, and a store that no longer
opened.

Two designs were offered. Making every write path multi-process safe means a cross-process lock
around each append and publication, re-reading the object map, the unflushed index entries (which
exist only in the writer's memtable, so a reader would have to rescan the current pack), the
remote map, the subject index and the pack tail under it, unique temporary names, and the same
treatment for the repository journal, checkpoints, and leases layered on the store. It would put a
re-validation on every read and write and would still leave a service's own caches (lake's search
segments and registry indexes) stale. The hand-off serialises whole processes instead: the root
has exactly one writer at any instant, the in-memory state stays authoritative for as long as the
lock is held, and the successor rebuilds it from disk on open, exactly as after a crash. That is
the same recovery path the crash simulator already proves, so the change adds no new on-disk
state beyond the lock file.

**A rollout without an unavailable window.** The hand-off removes the stop-then-start gap only if
the successor is reachable before it owns the root:

1. The successor starts, binds its listener, and passes readiness without opening the store.
2. It opens the store with `exclusive_owner` and an `owner_wait` longer than the predecessor's drain
   period. Requests that need the store wait for that open instead of failing.
3. The predecessor receives its stop signal, stops accepting, finishes in-flight work, and destroys
   its store (or is killed); the lock is released.
4. The successor's open succeeds within one retry interval (5 ms, doubling to 100 ms), recovers any
   torn pack tail, and serves the waiting requests.

Clients see added latency for the length of the drain and no refusals, provided the wait outlasts
the drain. The lock needs a filesystem whose `flock` is honoured between the processes: a local
block filesystem shared by two processes on one host is; a network filesystem must provide it
between its clients.

`store::close()` destroys the store and then awaits the owner's release, so a predecessor can hand
over at a known point instead of relying on destruction; the handle is empty afterwards.

### Injected owners and fencing

**What (liquid #327).** `store_options::owner` injects the owner lock as a `store_owner`, so a store
whose backend has no local root (lake's workspace stores on Zetta's object capability,
oramasearch/zetta#1005) is still owned by one holder at a time. Setting it implies exclusive
ownership; with `exclusive_owner` and no `owner`, a disk backend keeps the flock above, which is
itself the disk implementation of the interface (`make_disk_store_owner`).

```cpp
class store_owner {
  virtual runtime::task<owner_grant> acquire(std::string holder, std::chrono::milliseconds wait) = 0;
  virtual runtime::task<void> release() = 0;
};
```

- `acquire` waits up to `owner_wait` for the current holder and then fails with `errc::locked`
  naming it. The store passes `holder` as `pid <pid> on host <host> since <UTC time>`, the words the
  disk refusal uses, for the owner to record.
- The `owner_grant` it returns carries `fence()`, an opaque token that rises with every new holder,
  and `lost()`. Copies share one state: the owner keeps a copy and calls `revoke()` when it learns
  the holder may no longer write (a lease renewal failed or another holder took it).
- `release` gives the store up. `store::close()` awaits it. A store destroyed without `close()`
  revokes the grant and starts the release on the runtime, so a waiting successor still proceeds;
  the disk owner is released synchronously by its destructor as before. One owner object serves one
  open store at a time.

**The fence travels in a backend view.** After acquiring, open calls `backend->fenced(fence)` once
and sends every later backend call through the returned view, wrapped so a lost grant is enforced
locally. The view shares the backend's objects; a remote implementation attaches the fence to every
mutation and the remote refuses one whose fence is not the current holder's with `errc::locked`.
A view rather than a `set_fence` call on the backend: two store handles in one process may share a
backend instance (a predecessor and its successor during a hand-off), and a fence set on the shared
instance would let the predecessor write with the successor's token. The view also reaches
repository state written through the store's configured backend (leases, and the repository keys
of liquid #326), because the store's configuration names the wrapped view. A backend that cannot
carry a fence keeps the default `fenced`, and an open with an injected owner on it is
`errc::unsupported` after the owner is released again. The disk backend's view ignores the token:
the disk owner's fence is zero and the kernel lock already excludes every other writer. An owner
whose grant can lapse while its holder runs therefore needs a backend that enforces the fence.

**A lost grant.** Once the grant is lost, by the owner's `revoke()` or because the remote refused
the fence (the wrapper revokes the grant on any `errc::locked` from the view), every later
mutation (`put`, `put_if_absent`, `put_range`, `remove`, `barrier`) fails with `errc::locked` without reaching the
remote, and the store must be closed. Reads (`get_range`, `list`) fail the same way unless the
caller set `store_options::read_after_owner_lost`. The operation that discovered the loss fails
part way: its earlier writes are what any interrupted write leaves (an unsealed pack tail with
whole frames, unpublished object maps), and the successor's open recovers them exactly as after a
crash. The remote refusal is what makes this safe; the local check only stops a holder that already
knows it lost from asking.

**Adding a backend operation.** The wrapper forwards each `store_backend` operation with the
lost-grant check; `put_if_absent` (liquid #326) is forwarded as a mutation, so a repository's
conditional create also carries the fence and is refused once the grant is lost. A new mutating
operation must be added to the wrapper in `src/store/owner.cpp` the same way, and a remote backend's
view must carry the fence on it.

**Verify.** `tests/store/test_owner.cpp` (`StoreOwner.*`). In-process cases cover refusal, the
bounded wait expiring, a waiting open taking over when the owner is destroyed, and option
validation. Injected-owner cases run on a remote-shaped test backend (objects plus one lease whose
fence rises with each holder, refusing a mutation with a stale fence): a successor waits and takes
over when the predecessor closes, with a higher fence; a bounded wait expires; a holder whose lease
lapses in the middle of a multi-chunk put has that write refused by the remote and its later writes
and reads refused locally, while the successor opens, passes a collection dry run, reads every
acknowledged object and none of the interrupted one, and keeps them all through a full collection;
a grant the owner revokes refuses the next mutation without reaching the remote, permits reads
when opted in, and a store destroyed without `close()` still releases to a waiting successor; a
backend without `fenced` is `unsupported` and the owner is released; the disk owner can be
injected and released by `close()`; a stale holder's `put_if_absent` is refused by the remote and
then locally; and a shared-store repository's state keys are all written through the fenced view
with the owner's fence, its create is a conditional create, and its next write after the lease
lapses is refused. Process cases run `liquid-store-owner-fixture` as real processes: four writers started
together on one root must take turns and every acknowledged write must survive; and a writer that
is killed with `SIGKILL` in the middle of its write stream while a successor waits must hand the
root over, after which the successor writes. Each process case then checks the store: every frame
of every pack decodes and matches the digest its table records, the store reopens, every
acknowledged object from every process reads back byte for byte, and a full collection over leases
on all of them keeps every one. Without the lock both process cases fail.

## Whole-repository transfer

**What (liquid #328).** `repository::transfer(from, into, identifier, options)` copies the
repository that `identifier` names on the shared store `from` into the shared store `into`, both
open in one process, and returns a `repository_transfer_report`. It copies:

- every object reachable from the repository: its checkpoint and journal documents and those of its
  sessions, everything those documents name (the working tree, refs, history, directory documents,
  conflicts, operations, signed drips, metadata such as symbol records), and everything those
  objects reference in turn through the store's reference registry, so trees, changes, blob records,
  chunks, build records, attestations about a copied object, and the whole-content object of each
  blob; Drive files, models and datasets are repository files and metadata, so they travel the same
  way;
- the repository's state keys under `repositories/<id-hash>/`: `LIQUID_REPO`, `LIQUID_JOURNAL`,
  `sessions/<id>/…` and `replication/<source>.cursor` (the structural-merge parse graph is a cache
  and is skipped);
- the repository's lease records (owner `repository:<id>\n…`), so the destination's collector
  retains what the source's retained.

Every digest is kept. A chunk is read from the source (resolving delta bases), verified against its
digest, and appended to the destination's packs under the same identity as an ordinary frame. An
object is published to the destination's object map with the source's chunk list only after every
chunk has been read in order, the whole-content digest recomputed from them, and the chunk-list
descriptor recomputed and matched. Remote registrations (`remotes.meta`) are copied as
registrations, with whatever chunks the source holds locally. The destination is therefore equal
to the source in content digests, and an object it holds is readable with the destination's own
verification.

**Why (lake#380).** Lake moves each workspace's repositories from the space's disk
store into the workspace's own store, which lives on a remote backend over the workspace Zetta
(liquid #326, #327). `replicate_from` does this between two instances over HTTP; the move needs it
locally between two `store` handles, resumable and checkable before lake switches the repository
over.

**Order.** The walk is depth first and copies an object only after everything it references, so an
object present in the destination already has everything it reaches. Object metadata is published
every `batch_objects` objects (index flush, then the object map after a data barrier) and at the
end. Then the lease records are written, then a barrier, then every state key but the repository's
own `LIQUID_REPO` (state keys the destination holds and the source does not, such as a journal the
source has since checkpointed, are removed first), then a barrier, then `LIQUID_REPO`, then a
barrier. A destination interrupted at any point therefore has no repository, or one whose every
object is present; it never has a `LIQUID_REPO` naming a missing object.

**Resume and idempotence.** A rerun after an interruption walks from the roots again and does not
descend into an identity the destination already holds in the form the source does (an object in
its object map, a chunk in its index, a registration). Pack frames written before the interruption
are recovered by the destination's next open and are found in its index, so a rerun appends only
what is missing; at most one batch of object metadata is lost. A rerun of a complete transfer
writes nothing: no frame, no index run, no object map, no state key, no lease
(`RepositoryTransfer.InterruptedCopyResumesAndVerifies` counts the destination's mutations).

**Verify.** With `repository_transfer_mode::verify` the transfer writes nothing. It walks the same
reachable set without skipping, and for every identity reads the destination's copy: a chunk is
loaded and its digest checked, an object's size, kind, chunk list, chunk sizes and descriptor must
equal the source's and its chunks must reassemble to its digest, and a registration must be equal.
It then compares every state key and lease record byte for byte, and reports a destination state
key the source lacks. Each mismatch is a `repository_transfer_difference` (a digest or key, and the
reason); `complete` is true only when there is none.

**Fencing.** Every destination write goes through `into`'s persistence backend, which for a store
opened with an injected owner is the fenced view (see
[Injected owners and fencing](#injected-owners-and-fencing)). A transfer whose owner loses its
grant fails with `errc::locked`; the successor opens the store with a higher fence, recovers the
pack tail, and reruns the transfer.

**Memory.** Content passes one chunk at a time (at most 128 KiB, plus the decompressed delta base
it needs). Structural records (trees, changes, blob records) and the repository documents are read
whole, as the repository itself reads them. The walk keeps one 33-byte key per identity visited and
the references of the objects on its current path, so its memory grows with the number of objects,
not with their bytes.

**Report.** `objects` counts identities examined, `objects_present` those the destination already
held (and so did not descend into), `objects_read` and `bytes_read` the chunks read from the source,
`objects_written` and `bytes_written` what was appended or registered, and `state_written` and
`leases_written` the keys written or removed. `missing` lists identities a stored record references
that the source does not hold: they cannot be copied, the destination lacks them exactly as the
source does, and the transfer still completes. A digest-shaped value in a repository document that
the source does not hold (an operation identifier, an object already collected) is not reported.

**Limits.** Encrypted objects are refused with `errc::unsupported`: copying them would need either
the source's trust-domain key in the destination or re-encryption under the destination's key, which
changes keyed identities and so every record that names them. Both stores must be shared stores
(a repository with its own checkout store is not a transfer source). The repository must not be
written while it is transferred; a write after the transfer read the source's state is not copied,
and a rerun copies it. Nothing else should write or collect the destination during the transfer:
copied objects are unreferenced until `LIQUID_REPO` is written, so a collection in between could
remove them (the rerun would copy them again).

**Verify (tests).** `tests/store/test_transfer.cpp` (`RepositoryTransfer.*`) builds a repository on
a disk store with history, a rename, a 6 MiB file streamed from disk, a second ref, an
uncheckpointed journal, a private session with its own write, a replication cursor, and an unrelated
repository beside it, and transfers it into the remote-shaped test backend of liquid #327 held by
an injected owner, with one-MiB packs so the copy spans several. An uninterrupted copy measures the
destination's mutations; then, for interruptions at 10, 35, 60, 90 and 99 percent of them, the
owner's lease lapses mid-copy, the remote refuses the next write and the transfer fails with
`errc::locked` leaving no `LIQUID_REPO`; a successor with a higher fence reruns it, appending fewer
bytes than the whole copy and writing `LIQUID_REPO` with its own fence; verify reports no
difference; a third run writes nothing; and after a reopen the destination verifies again and opens
the repository with byte-identical files (the streamed file included), the same refs, history,
drips, replication cursor and session write, and passes a collection dry run. A second test flips
one byte in a destination pack and verify reports the object that no longer verifies, changes a
copied cursor and verify reports that key, and verify of an empty destination reports the missing
state. A third covers the refusals: a repository absent from the source (`errc::not_found`), one
store as both ends, and a zero batch (`errc::invalid_argument`).

## Tuning and verification

`store_options::pack_size` controls pack rollover. The index options default to 1,048,576 memtable
entries, 10 bloom bits per key, and a four-run compaction threshold. Change these only with tests
for mapped reopen, rebuild, read integrity, and compaction.

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L large --output-on-failure
ctest --test-dir build -L large-40g --output-on-failure
```

The default tests cover deterministic boundaries, range reads, deduplication, corruption, reopen,
unknown versions, index reconstruction, run corruption, compaction, and separate SHA-256 run-set
lookup. `index-write-once` and `store-write-once` repeat the index, store, collection and
repository transfer suites over the write-once backend; `ChunkIndex.CompactionWritesEachRunOnce` and
`ChunkIndex.TornMergeLeavesOldRunsAuthoritative` run over it in the default suite as well. The `large` label contains the 256 MiB streaming round trip; `large-40g` is the labelled
Phase 1 exit test.

The chunk index's compaction merges runs across suspension points. Nothing in that merge may point
into `algorithm_state::runs`: a flush that lands while it is suspended appends a run and reallocates
that vector. The merge copies each input's sequence, record count, record offset and mapped bytes
before it starts, and selects the runs it replaces by sequence rather than by index (issue #271).
