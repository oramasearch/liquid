# Content-defined chunk store

**Status: implemented for single-process local stores (liquid #1).** `store::open`, `put`,
`get`, `read`, and `contains` are Capy coroutines. Leases and collection remain unsupported
until liquid #8.

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
<root>/tmp/
```

`open` creates missing directories and refuses a marker other than format 1 with
`errc::unsupported`. A pack begins with its id. A chunk body is an algorithm byte, 32-byte digest,
raw and compressed lengths, reserved bytes, and an individually zstd-compressed payload. The pack
ends with a sorted chunk table and a footer containing the table location and checksum. The table
carries the algorithm tag, so it remains the source for rebuilding index run sets. A damaged pack
footer or table is a `corrupt` error; its entries are never silently ignored.

The current pack keeps a complete footer after each append. The next append removes the previous
table and footer, appends the new compressed chunk, and writes a replacement table and footer.
Packs roll at `store_options::pack_size`, or 64 MiB when it is zero.

## Chunking and integrity

FastCDC uses a deterministic xorshift-derived 256-entry gear table seeded with
`0x9e3779b97f4a7c15`. It cuts at 8 KiB minimum, 64 KiB target, and 128 KiB maximum. The algorithm
is platform-independent. `put` produces BLAKE3 identities today. The index and pack table retain
algorithm information for BLAKE3 and SHA-256, allowing a later liquid #44 writer to add SHA-256
without a format change. Every `get` and `read` rehashes raw chunk bytes; a mismatch returns
`errc::corrupt` with the digest in `error::context`.

`read` loads only the chunk headers and compressed byte ranges that overlap the caller's range.
Pack file reads and writes use Corosio `random_access_file`; synchronous directory creation and
atomic manifest rename are limited to filesystem metadata operations. Corosio open failures are
converted to `errc::io` at the private boundary.

## Index

The index is liquid-owned. Its portable records are exactly 48 bytes, serialized as digest bytes
(32), pack id (u32), offset (u64), and compressed length (u32). The digest comes first, so records
sort lexicographically by digest bytes.

An algorithm owns its own memtable and immutable run set. The run header carries that algorithm,
a 256-way first-byte fanout table, a 10-bit-per-key bloom filter, record count, and checksum.
`MANIFEST` lists live runs with their algorithm and the highest indexed pack id. Lookups check the
algorithm's memtable, then runs newest first. Flushing sorts the memtable and writes a new immutable
run; after more than four runs the store merges them, writes and records the replacement, then
removes old runs. The manifest is written to a temporary file, synchronized, and atomically renamed.

If the manifest or a run header is absent or corrupt, `open` reconstructs the run sets from pack
tables. Deleting `index/` is therefore recoverable. There is no index write-ahead log.

Concurrent coroutines on one store serialize mutation with `capy::async_mutex`. Reads use immutable
runs and are not given that mutation lock. Cross-process access is outside this issue's contract.

## Tuning and verification

`store_options::pack_size` controls pack rollover. The chunk bounds, bloom density (10 bits/key),
and compaction threshold (four runs) are format or engine constants for this format version. Change
them only with tests for rebuild, read integrity, and compaction.

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L large --output-on-failure
```

The default tests cover deterministic boundaries, range reads, deduplication, corruption, reopen,
unknown versions, index reconstruction, run corruption, compaction, and separate SHA-256 run-set
lookup. The `large` label contains the 256 MiB round trip.
