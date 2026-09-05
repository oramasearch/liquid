# Search index tests

`test_index.cpp` covers the public repository search and plan surfaces, content-addressed blob
reuse, positional substring matching, immutable-segment publication under a concurrent reader,
daemon drip delivery, and duplicate cursor suppression for the search target. Planner cases pin
scan below and trigrams above the measured threshold, compare complete literal and regex hit
identities between executors, and reject a stale index after a captured drip.
The test executable compiles the private index implementation directly so shared-library builds do
not export internal test helpers.

Index fixtures allocate their named directories beneath the unique process root specified in the
parent [`README.md`](../README.md).

`measure_index.cpp` builds `.liquid/index/` from a checkout and reports file, unique-blob, byte,
build-time, and mean-query figures. It also forces mapped scan and verified-trigram execution over
10 to 4,000 files for the crossover curve. CMake excludes the measurement target from normal
builds.
