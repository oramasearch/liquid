# Chunk-index benchmark: RocksDB, LMDB, and liquid

This benchmark measures the candidate engines and liquid's own index format: the map from a 32-byte chunk
digest to a 16-byte `(pack id, offset, length)` location. It produces measurements for the
maintainer's later decision; it does not choose an engine and does not add either engine to
libliquid.

## Reproducible workload

The executable is `bench/chunk-index/chunk_index_bench.cpp`. It has one client thread and runs the
same ordered phases against every engine, including `--engine liquid`:

1. Generate `N` 32-byte keys from the standard `std::mt19937_64` stream seeded with `1`, then insert
   them in batches of 10,000. RocksDB uses one `WriteBatch`; LMDB uses one write transaction per
   batch.
2. Run 1,000,000 point lookups, alternating between a random existing key and an independently
   generated random key. Any unexpected presence result fails the run.
3. Run 10,000 groups of 64 reads starting from a random existing key. RocksDB and LMDB use cursors;
   liquid uses 64 mapped-index lookups because its private index has no cursor API. Every measured
   operation visits exactly 64 entries.
4. Flush, close, attempt an unprivileged OS-cache eviction, reopen, and run the first 100,000 point
   lookups again.
5. Close and report final directory bytes, bytes written, write amplification, and process peak RSS.

Each value is encoded little-endian as a `u32` pack id, `u64` byte offset, and `u32` length. The
generated locations model 512 64-KiB chunks per pack. Key words are expanded to bytes explicitly,
so seed `1` produces the same keys regardless of host endianness. Point-query and range-start
selection use fixed auxiliary seeds `2` and `3`; both engines therefore receive the same keys and
queries.

`ops/s` counts inserted keys, individual point lookups, or complete 64-entry scans. Point and scan
percentiles are per operation. Because an insert is only committed as part of a batch, insert p50
and p99 are the batch duration divided by that batch's key count, with percentiles across batches.
The phase wall clock includes latency sampling and correctness checks.

RocksDB bytes written are its `rocksdb.bytes.written` ticker. LMDB does not expose an equivalent
counter, so its final directory size is used for bytes written as required by this benchmark.
Write amplification divides that engine-specific figure by `N * 48`. The two amplification values
are useful within this workload but are not identical accounting methods.

## Build and tuning

The benchmark is isolated behind `LIQUID_BUILD_BENCH`, which defaults to OFF. A benchmark-enabled
single-config build selects Release when no build type was supplied. It fetches RocksDB `v10.10.1`
as a static library with gflags, tests, tools, benchmark tools, and external compression libraries
disabled. It fetches LMDB `LMDB_0.9.36` and compiles `mdb.c` and `midl.c` directly.

```sh
cmake -S . -B build-bench -G Ninja -DLIQUID_BUILD_BENCH=ON
cmake --build build-bench --target chunk-index-bench
```

Both RocksDB runs use no compression because compression libraries are disabled. The default pass
otherwise leaves RocksDB options alone. Its tuning pass adds a 10-bit whole-key bloom filter and a
512-MiB LRU block cache:

```sh
./build-bench/bench/chunk-index/chunk-index-bench --n 10000000 \
  --engine rocksdb --dir /tmp/cib-10m-rocksdb-default
./build-bench/bench/chunk-index/chunk-index-bench --n 10000000 \
  --engine rocksdb --dir /tmp/cib-10m-rocksdb-tuned --tuning tuned
```

LMDB requires a map ceiling before the population can be inserted. By default the benchmark uses
the larger of 512 MiB or `N * 256 + 256 MiB`; this is virtual address space, not preallocated file
size. Both LMDB passes use that same ceiling. The baseline preserves LMDB's synchronous commit
behavior; the tuning pass enables `MDB_NOSYNC`, then performs a forced sync after timed work and
before close so the final files can be measured:

```sh
./build-bench/bench/chunk-index/chunk-index-bench --n 10000000 \
  --engine lmdb --dir /tmp/cib-10m-lmdb-sync
./build-bench/bench/chunk-index/chunk-index-bench --n 10000000 \
  --engine lmdb --dir /tmp/cib-10m-lmdb-nosync --tuning nosync
```

`MDB_NOSYNC` changes crash durability: an operating-system crash can lose recent transactions. It
is reported as a distinct configuration, not as a free throughput improvement. `--lmdb-map-mib`
overrides the derived ceiling. Every `--dir` must be absent or empty; the program never erases an
existing database.

## Recorded machine

Measurements were taken on 2026-09-03 with no other benchmark process running.

| Item | Value |
|---|---|
| CPU | Apple M3 Max, arm64 |
| Memory | 68,719,476,736 bytes (64 GiB) |
| Disk | Internal 2.0-TB solid-state device, Apple Fabric; 1,995,218,165,760 bytes |
| OS | macOS 14.6.1 (23G93) |
| Compiler | Homebrew Clang 22.1.8 |
| Build tools | CMake 4.3.4, Ninja 1.13.2, Release |

macOS does not offer an unprivileged system page-cache eviction command. Every run reports
`cache_drop=unavailable_without_root_on_macos`, so the close/reopen phase below is a reopened,
warm-OS-cache measurement rather than a true cold-disk measurement.

## liquid results

`--engine liquid` drives `liquid::store_detail::chunk_index`, not a benchmark-only engine. It
uses the production 48-byte digest-first records, a 1,048,576-entry hash memtable, 10-bit bloom
filters, 65,536-bucket fanout, and four-run size-tiered compaction. The insert phase includes
threshold-triggered flushes and compactions. The reopen phase closes the index, maps runs again,
and performs the point lookups.

| N | Bulk insert ops/s | Point lookup ops/s | Range scan (64) ops/s | Reopen point lookup ops/s | On-disk bytes | Peak RSS bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000,000 | 5,253,317.13 | 2,855,164.29 | 20,774.91 | 3,790,583.02 | 49,512,236 | 316,669,952 |
| 10,000,000 | 2,099,543.19 | 1,070,078.74 | 14,877.68 | 892,647.60 | 494,073,296 | 1,137,377,280 |

Both runs used the recorded Apple M3 Max machine and reported
`cache_drop=unavailable_without_root_on_macos`. At 10M, liquid meets the required 1.16M inserts/s
(2.100M), 438k point lookups/s (1.070M), and 589 MB disk limit (494.1 MB).

## Results at N = 10,000,000

| Engine | Configuration | Phase | ops/s | p50 (us) | p99 (us) |
|---|---|---|---:|---:|---:|
| RocksDB | default | bulk insert | 798,765.03 | 1.258 | 2.338 |
| RocksDB | default | point lookup | 70,144.41 | 13.625 | 31.834 |
| RocksDB | default | range scan (64) | 37,217.78 | 25.584 | 48.042 |
| RocksDB | default | reopen point lookup | 89,952.86 | 7.917 | 43.542 |
| RocksDB | bloom + 512-MiB cache | bulk insert | 1,157,599.37 | 0.856 | 1.587 |
| RocksDB | bloom + 512-MiB cache | point lookup | 437,666.84 | 2.084 | 7.041 |
| RocksDB | bloom + 512-MiB cache | range scan (64) | 66,779.39 | 14.584 | 20.417 |
| RocksDB | bloom + 512-MiB cache | reopen point lookup | 431,503.57 | 2.125 | 7.291 |
| LMDB | map 2,697 MiB, sync | bulk insert | 55,038.02 | 20.073 | 25.558 |
| LMDB | map 2,697 MiB, sync | point lookup | 935,297.38 | 1.000 | 4.042 |
| LMDB | map 2,697 MiB, sync | range scan (64) | 454,816.79 | 2.125 | 4.708 |
| LMDB | map 2,697 MiB, sync | reopen point lookup | 759,018.80 | 1.167 | 2.375 |
| LMDB | map 2,697 MiB, `MDB_NOSYNC` | bulk insert | 268,873.53 | 3.706 | 5.911 |
| LMDB | map 2,697 MiB, `MDB_NOSYNC` | point lookup | 1,040,100.51 | 0.958 | 1.375 |
| LMDB | map 2,697 MiB, `MDB_NOSYNC` | range scan (64) | 512,377.11 | 1.916 | 2.958 |
| LMDB | map 2,697 MiB, `MDB_NOSYNC` | reopen point lookup | 762,016.76 | 1.167 | 2.166 |
| liquid | mapped run, 10-bit bloom | bulk insert | 2,099,543.19 | 0.155 | 0.373 |
| liquid | mapped run, 10-bit bloom | point lookup | 1,070,078.74 | 0.750 | 5.542 |
| liquid | mapped run, 10-bit bloom | range scan (64) | 14,877.68 | 62.750 | 123.500 |
| liquid | mapped run, 10-bit bloom | reopen point lookup | 892,647.60 | 0.792 | 4.166 |

| Engine | Configuration | On-disk bytes | Bytes written | Write amplification | Peak RSS bytes |
|---|---|---:|---:|---:|---:|
| RocksDB | default | 576,489,329 | 510,012,000 | 1.062525 | 855,064,576 |
| RocksDB | bloom + 512-MiB cache | 589,291,963 | 510,012,000 | 1.062525 | 1,485,291,520 |
| LMDB | map 2,697 MiB, sync | 1,173,626,880 | 1,173,626,880 | 2.445056 | 1,703,051,264 |
| LMDB | map 2,697 MiB, `MDB_NOSYNC` | 1,173,626,880 | 1,173,626,880 | 2.445056 | 1,702,051,840 |
| liquid | mapped run, 10-bit bloom | 494,073,296 | 760,414,048 | 1.584196 | 1,137,377,280 |

## Run N = 100,000,000 on Linux

Use a 64-bit Linux host with a local SSD and fresh directories on that SSD. Record the host before
the runs, then build and run all four configurations:

```sh
uname -a
lscpu
free -b
lsblk -d -o NAME,MODEL,TRAN,ROTA,SIZE

cmake -S . -B build-bench -G Ninja -DLIQUID_BUILD_BENCH=ON
cmake --build build-bench --target chunk-index-bench

./build-bench/bench/chunk-index/chunk-index-bench --n 100000000 \
  --engine rocksdb --dir /mnt/benchmark/cib-100m-rocksdb-default
./build-bench/bench/chunk-index/chunk-index-bench --n 100000000 \
  --engine rocksdb --dir /mnt/benchmark/cib-100m-rocksdb-tuned --tuning tuned
./build-bench/bench/chunk-index/chunk-index-bench --n 100000000 \
  --engine lmdb --dir /mnt/benchmark/cib-100m-lmdb-sync --lmdb-map-mib 32768
./build-bench/bench/chunk-index/chunk-index-bench --n 100000000 \
  --engine lmdb --dir /mnt/benchmark/cib-100m-lmdb-nosync --tuning nosync \
  --lmdb-map-mib 32768
```

On Linux the executable opens every regular database file and issues
`posix_fadvise(..., POSIX_FADV_DONTNEED)` after close. Verify the final line says
`cache_drop=linux_posix_fadvise_files=<nonzero count>`; otherwise the reopen numbers are not
confirmed cold-cache results. Do not reuse directories between runs.

## Decision

**liquid's own index, built from scratch** (Steve, 2026-09-03), recorded in `CLAUDE.md`
*Decisions* 8. Neither engine ships inside liquid; these measurements are the **bar** the
from-scratch index must meet at 10M entries: RocksDB-tuned's insert rate (1.16M/s) and point rate
(438k/s) and no more than its on-disk footprint (589 MB). `--engine liquid` joins this benchmark
when the index exists so the comparison is run the same way. (An earlier decision the same day
chose RocksDB from these numbers; it was withdrawn in favour of owning the engine.)

## What the numbers say

At this population, RocksDB's default insert rate was 14.5 times LMDB's synchronous rate. Enabling
`MDB_NOSYNC` raised LMDB insert throughput by 4.9 times, while retaining the durability limitation
described above; RocksDB's tuned insert rate remained 4.3 times that result.

For reads, LMDB's synchronous baseline delivered 13.3 times the point-lookup throughput and 12.2
times the 64-entry scan throughput of default RocksDB. The RocksDB bloom/cache pass raised its
point-lookup throughput by 6.2 times and scan throughput by 1.8 times. LMDB with `MDB_NOSYNC` was
still 2.4 times the tuned RocksDB point rate and 7.7 times its scan rate. The reopen comparison has
the same direction, but it must not be read as cold-disk behavior on this macOS host.

RocksDB occupied 576.5 MB by default; the bloom-filter pass added 12.8 MB. LMDB occupied 1.174 GB,
about 2.0 times the default RocksDB directory. The reported amplification was 1.06 for RocksDB and
2.45 for LMDB, subject to the different accounting methods above. Peak RSS was 855 MB for default
RocksDB, 1.485 GB for tuned RocksDB, and about 1.70 GB for either LMDB mode.

These numbers describe one 10-million-entry macOS population. A decision still needs the documented
100-million-entry Linux runs, a true cold-cache comparison, and the maintainer's durability and
operational criteria.
