# Store tests

`test_store.cpp` covers deterministic FastCDC boundaries, local edit stability, small-object
round trips, range reads, deduplication, corruption, reopen and format refusal.
`test_store_large.cpp` is the `large`-labelled 256 MiB round trip. `http_range_test.py` verifies
that an indexed compressed record can be fetched exactly through a plain HTTP byte-range request.
`chunker_fuzz.cpp` backs the optional `LIQUID_BUILD_FUZZ` target.
`store_measure.cpp` backs the optional POSIX measurement target used for documented deduplication,
throughput and peak-RSS runs.
