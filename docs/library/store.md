# Content-defined chunk store

**Status: implemented for single-process local stores (liquid #1, #7, #8, #46, #116).**
`store::open`, `put`, `put_file`, `get`, `get_to_file`, `read`, and `contains` are Capy
coroutines. The same operations support encrypted objects when supplied a keyring. Leased roots
and epoch collection are implemented as Capy coroutines; see [gc.md](gc.md). Collection also
compacts sparse packs without recompressing frames. The Phase 1 large-input exit criterion is met.

## What and why

The store accepts bytes and returns a BLAKE3 digest for the complete input. It splits content
with FastCDC, stores raw chunks once, and records the input's ordered chunk list. Reading verifies
each chunk's digest after decompression. The layout lets source, build output, and large binaries
share chunks without a file-per-object bottleneck.

## Layout and pack format

```
<root>/LIQUID_STORE       format marker: `liquid-store 1\n`
<root>/packs/pack-<id>.pack
<root>/packs/objects.meta ordered input digest → chunk-list descriptors
<root>/index/MANIFEST
<root>/index/run-<seq>.idx
<root>/leases/lease-<id>-<generation>.lease
<root>/gc/MANIFEST       committed epoch and per-pack tombstone bitmaps
<root>/keys/<key-id>.wrapped   encrypted trust-domain data key
<root>/tmp/
```

`open` creates missing directories and refuses a marker other than format 1 with
`errc::unsupported`. A pack begins with its id. Every following chunk is one append-only frame:
an eight-byte frame magic, raw and compressed lengths (`u32` each), an algorithm byte, the 32-byte
digest, and an individually zstd-compressed payload. A sealed pack ends with a sorted chunk table
and a footer containing the table location and checksum. The table carries the algorithm tag, so it
remains the source for rebuilding index run sets.

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
key. The object map format is now `LQOBJ4`: each record carries every chunk's raw size in addition
to `LQOBJ3`'s descriptor digest and object kind and `LQOBJ2`'s encryption flag and optional
length-prefixed key id. Those sizes let `read` jump to the first chunk overlapping a large offset
without loading preceding chunks. A zero
descriptor algorithm is the sentinel for legacy records that predate stored chunk-list objects.
Its chunk-list descriptor contains the same key id and is encrypted too. `LQOBJ1` plaintext and `LQOBJ2`
encrypted metadata remain readable, as does `LQOBJ3`; the next metadata write fills legacy chunk
sizes from pack tables before publishing `LQOBJ4`.

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

`read` loads only the chunk headers and compressed byte ranges that overlap the caller's range.
Pack file reads and writes use Corosio `random_access_file`; synchronous directory creation and
atomic manifest rename are limited to filesystem metadata operations. Corosio open failures are
converted to `errc::io` at the private boundary.

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
updated for each window and finalized only after the last byte. Each completed chunk is
deduplicated and framed immediately; the whole-object chunk-list descriptor and `LQOBJ4` metadata
are published at the end.

`store::get_to_file` loads and verifies one chunk at a time and writes it with Corosio
`random_access_file`. It incrementally verifies the whole-object digest before acknowledging the
export. A one-chunk cache avoids reopening and decompressing consecutive duplicate chunks while
remaining bounded. `repository::write_file` imports through `put_file`, derives its canonical blob
record from that stored ordered chunk list, and records one drip. The in-memory repository write
uses the same stored list, so both paths produce the same blob record. Raw blob chunks remain
readable under encryption through their parent object's trust-domain key.

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

## Pack compaction

`collection_options::compact_below` defaults to `0.5`. A partially dead pack is compacted when
its live frame-byte fraction is strictly below that value; `0` disables compaction.
`collection_options::compact_only` uses the committed tombstone manifest without another mark and
sweep. The CLI exposes that pass as `liquid gc --compact`; `--compact-below <fraction>` changes
the threshold for either collection mode.

For each selected pack, the collector holds its mutation lock, records a pending compaction
marker, then copies each non-tombstoned physical frame into a new sealed pack. A copied frame is
its original header and compressed (or encrypted) payload byte-for-byte: compaction never
decompresses, recompresses, or recalculates its identity. The new locations are inserted into a
newest index run. The replacement pack and run are only preparation; the collection manifest
rename is the commit point. That manifest replaces the old pack's partial bitmap with an all-dead
bitmap. Only after the rename does the index manifest publish the new run and does the collector
remove the old pack and its pending marker.

Recovery reads a pending marker before opening the index. Before the collection manifest rename,
it removes the replacement pack; after it, it removes the old pack. A crash between the collection
manifest and index-manifest updates rebuilds the index from packs and the committed tombstones.
Thus an uncommitted replacement never survives the next open and a committed replacement is found
even if index publication did not finish.

Readers do not take the mutation lock. A reader that resolved an old location before the switch
can still read the old pack until its later removal. On POSIX, a reader that already opened the
file keeps a valid mapping after unlink. On Windows, a removal blocked by an open mapping is left
for the next store open through the pending marker.

## Index

The index is liquid-owned. Its portable records are exactly 48 bytes, serialized as digest bytes
(32), pack id (u32), offset (u64), and compressed length (u32). The digest comes first, so records
sort lexicographically by digest bytes.

An algorithm owns an `std::unordered_map` memtable and immutable run set. The default memtable
limit is 1,048,576 entries; inserts and memtable lookups are O(1), and the table is flushed at
that limit or when its pack seals. The run header carries the algorithm, record count, a
two-byte (65,536-bucket) fanout table of u32 cumulative counts, a 10-bit-per-key bloom filter,
and a checksum.

`MANIFEST` lists live runs with their algorithm and the highest indexed pack id. `open` maps each
immutable run read-only through the private cross-platform mapping helper; it does not read record
bytes. Lookups check the algorithm's memtable, then mapped runs newest first. A bloom hit selects
the two-byte fanout slice, and `std::lower_bound` searches the mapped 48-byte records directly.
The index therefore issues no file I/O on its lookup path. Mapping keeps per-run process memory
bounded by the 256 KiB fanout header plus the bloom filter; mapped record pages remain managed by
the operating system.

Runs are size-tiered by record count. When more than four runs share a tier, compaction performs a
streaming k-way merge by iterating mapped input records and incrementally writing the replacement
with Corosio through bounded 4 MiB buffers. It does not load input runs into memory. The replacement is synchronized and
recorded in the manifest before the old runs are unmapped and removed. The manifest itself is
written to a temporary file, synchronized, and atomically renamed.

If the manifest or a run header is absent or corrupt, `open` reconstructs the run sets from pack
tables. Deleting `index/` is therefore recoverable. There is no index write-ahead log.

Concurrent coroutines on one store serialize mutation with `capy::async_mutex`. Reads use immutable
runs and are not given that mutation lock. Cross-process access is outside this issue's contract.

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
lookup. The `large` label contains the 256 MiB streaming round trip; `large-40g` is the labelled
Phase 1 exit test.
