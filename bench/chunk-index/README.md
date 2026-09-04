# Chunk-index benchmark

This directory contains the optional RocksDB/LMDB/liquid chunk-index benchmark. It is isolated behind
`LIQUID_BUILD_BENCH=ON`; the default configure does not fetch or build either engine.

The workload, commands, dependency versions, measurement definitions, and recorded results are in
[`docs/library/chunk-index-benchmark.md`](../../docs/library/chunk-index-benchmark.md).
