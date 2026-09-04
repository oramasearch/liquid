# Chunk store

**What.** `liquid::store` is the content-defined store behind libliquid. `open`, `put`, `get`,
`read`, and `contains` are implemented. Input bytes are divided by FastCDC, identified by a
digest of the raw bytes, compressed independently into packs, and located through RocksDB.
`put` returns a digest of the complete input and records the ordered chunk list needed to
reconstruct it. Leases and collection remain owned by liquid #8 and return
`errc::unsupported`.

**Why.** Source, build outputs, packages, images, datasets, and indexes need one deduplication
unit without turning every chunk into a separate filesystem object. Chunks provide identity and
reuse; packs provide storage and transfer efficiency; the index provides point lookups at the
population described in [the index benchmark](chunk-index-benchmark.md).

## Public behavior

`store::open(root, options)` creates or opens the on-disk layout. A default-constructed store is
not open; data operations on it return `errc::invalid_argument`. Public calls catch internal
exceptions and return `errc::internal`.

`put(bytes)` hashes the complete input using `store_options::hash_algorithm`, cuts it into
chunks, writes only absent chunks, stores a chunked object manifest, and returns the complete
input digest. BLAKE3 is the default. SHA-256 input can also be written, and a store opened with
either configured algorithm can resolve an existing digest carrying the other algorithm.

`get(digest)` loads the manifest and reconstructs the complete input. `read(digest, offset,
buffer)` loads only content chunks overlapping the requested range; it returns zero at or beyond
end of input. Manifest chunks are metadata and are loaded first. `contains(digest)` is a point
lookup in the object column family.

Every content and manifest chunk is decompressed independently and hashed before being returned.
A damaged record, digest mismatch, or invalid encoded length returns `errc::corrupt`; the
expected digest text is placed in `error::context`.

The contract in this issue is one process and one thread per store. No internal synchronization
or inter-process writer coordination is promised.

## Chunking

FastCDC uses these fixed parameters:

| Parameter | Value |
|---|---:|
| Minimum | 8 KiB |
| Target | 64 KiB |
| Maximum | 128 KiB |
| Early mask | 17 bits |
| Late mask | 15 bits |

The 256-entry gear table is generated at compile time by SplitMix64 from the fixed seed
`0x6c69717569642d31`. Byte conversion is explicit, so boundaries are identical across host
endianness and platforms. Tests assert exact repeatability and that a front insertion in a 4 MiB
input replaces at most a small number of chunk identities around the edit.

## On-disk layout

```
ROOT/
  LIQUID_STORE
  packs/
    0.pack
    0.table
  index/
    ... RocksDB files ...
  tmp/
```

`LIQUID_STORE` contains `LIQUID_STORE\nversion=1\n`. A marker with any other contents is an
unknown format and `open` returns `errc::unsupported`. A missing marker is created only when the
root is absent or empty, preventing an unrelated directory from being adopted accidentally.

Pack ids are unsigned 32-bit decimal names. The default target is 64 MiB; callers may choose a
target from 16 MiB through 64 MiB with `store_options::pack_size`. A record that would cross the
target starts the next pack. Older packs are then never appended again.

Each chunk is one zstd frame and can be fetched or decompressed without its neighbors. The pack
record is:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | `LCHK` |
| 4 | 1 | record version, currently 1 |
| 5 | 1 | digest algorithm |
| 6 | 2 | reserved, zero |
| 8 | 4 | raw length, little-endian |
| 12 | 4 | compressed length, little-endian |
| 16 | 32 | raw-chunk digest bytes |
| 48 | compressed length | one zstd frame |

Compression level 3, zstd long-distance matching, and the pinned library's private
`ZSTD_c_rsyncable` parameter are enabled. One zstd worker activates rsyncable mode. Independent
FastCDC records remain the primary stable, range-addressable units; the experimental parameter is
safe to pin here because zstd is private and fixed at version 1.5.7.

The sidecar begins with `LQTABLE1`, followed by fixed 49-byte entries: algorithm byte, 32 digest
bytes, offset u64, stored record length u32, and raw length u32, all integers little-endian. It is
the pack's chunk table and supports transfer tooling. RocksDB remains authoritative for lookup.

## Index and object manifests

RocksDB `v10.10.1` lives under `index/`. The default column family maps a 33-byte key—one
algorithm byte followed by 32 digest bytes—to this fixed 16-byte little-endian value:

| Bytes | Meaning |
|---:|---|
| 4 | pack id u32 |
| 8 | record offset u64 |
| 4 | stored record length u32 |

The `objects` column family uses the same 33-byte key for complete-input digests. Its value is a
small descriptor pointing to the chunks of an object manifest. A manifest records total input
length and the ordered `(algorithm, digest, raw length)` content-chunk list. Manifests are
themselves passed through the same FastCDC, digest, deduplication, pack, and chunk-index path.
This separate column family handles the valid case where a one-chunk input has the same digest as
its only content chunk.

Both column families use a 10-bit whole-key bloom filter. The default LRU block cache is 512 MiB;
`store_options::index_cache_size` overrides its byte size. RocksDB compression is disabled because
chunk payload compression belongs to packs. Writes are synced before a chunk or object is
reported indexed.

RocksDB `v10.10.1`, zstd `v1.5.7`, and BLAKE3 `1.8.7` are pinned FetchContent dependencies and
private static components of libliquid. Static libliquid archives merge their objects so an
installed consumer does not need third-party targets or headers. Shared builds link them with
hidden visibility. No third-party type reaches `include/liquid/`.

## Verification

The required static, shared, and 256 MiB proofs are:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON && cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
ctest --test-dir build -L large --output-on-failure
```

The test suite covers a 40-byte input, deterministic and edit-stable chunking, cross-boundary
reads, a repeated 10 MiB input with no pack growth, pack corruption, reopen, digest-algorithm
resolution, unknown format refusal, and the 256 MiB round trip. `store-pack-http-range` starts a
plain local HTTP server, fetches one indexed pack record with `Range`, and compares the response
with the exact local pack extent.

Build and smoke-run the chunker fuzz target with Clang:

```sh
cmake -S . -B build-fuzz -G Ninja -DLIQUID_BUILD_TESTS=OFF -DLIQUID_BUILD_CLI=OFF -DLIQUID_BUILD_FUZZ=ON
cmake --build build-fuzz --target liquid-chunker-fuzz
./build-fuzz/liquid-chunker-fuzz -runs=10000 -max_len=262144
```

## Recorded store measurements

Recorded 2026-09-03 on an Apple M3 Max with 64 GiB RAM, macOS 14.6.1, Homebrew Clang
22.1.8, and a Release build. `liquid-store-measure` memory-maps each input, calls public
`store::put`, reports timed ingest throughput and process peak RSS, and reads sidecars to report
unique stored chunks. Content-only deduplication is computed separately for datasets up to 2 GiB
so manifest overhead and zstd compression do not distort it.

| Dataset | Files | Input bytes | Unique content bytes | Content/input | Pack bytes | Throughput | Peak RSS bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| liquid `origin/main` at `39c9bf6b326de2113e6a20bb250f1657eadf5c21` | 147 | 2,159,737 | 2,159,708 | 0.999987 | 897,940 | 1.05924 MiB/s | 12,550,144 |
| Four 256 MiB binary revisions | 4 | 1,073,741,824 | 272,079,664 | 0.253394 | 272,762,647 | 50.7044 MiB/s | 302,088,192 |
| One sparse zero-filled 40 GiB file | 1 | 42,949,672,960 | not separately scanned | not separately scanned | 4,261 | 332.983 MiB/s | 20,824,408,064 |

The repository input was a `git archive` of the named revision. The binary dataset began with one
random 256 MiB file and three copies; one independent random 1 MiB region replaced bytes at 64,
128, and 192 MiB in the three revisions. Its 0.253394 content ratio is the deduplication result;
the pack ratio of 0.254030 includes zstd and manifests.

The 40 GiB file was sparse and read as zeroes. The run processed every byte through complete-input
hashing and FastCDC, but repeated chunks reduced stored pack bytes to 4,261. Peak RSS includes
resident pages from the 40 GiB read-only mapping; libliquid does not allocate or copy a buffer the
size of the input. Its input is the caller-owned span, and internal allocations scale with chunk
references, manifests, individual chunks, and the configured RocksDB cache.

Build the measurement tool with:

```sh
cmake -S . -B build-measure -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLIQUID_BUILD_TESTS=OFF -DLIQUID_BUILD_CLI=OFF \
  -DLIQUID_BUILD_STORE_MEASUREMENTS=ON
cmake --build build-measure --target liquid-store-measure
./build-measure/liquid-store-measure STORE INPUT...
```

## How to change

Changing chunk parameters or the gear seed changes chunk identities for future writes and must be
treated as a format decision, with boundary fixtures and migration analysis. Pack record or
sidecar changes require a version bump and compatibility reader. Index key or value changes need a
new column family or an offline migration; do not reinterpret existing fixed-width values.

Pack target size and RocksDB block-cache bytes are caller-tunable through `store_options`.
Compression level, chunk parameters, bloom bits, record encoding, and marker version are format
or implementation constants and require this page plus tests to change with the code.
