# Search index tests

`test_index.cpp` covers the public repository search surface, content-addressed blob reuse,
positional substring matching, immutable-segment publication under a concurrent reader, daemon
drip delivery, and duplicate cursor suppression for the search target.
The test executable compiles the private index implementation directly so shared-library builds do
not export internal test helpers.

`measure_index.cpp` builds `.liquid/index/` from a checkout and reports file, unique-blob, byte,
build-time, and mean-query figures. CMake excludes the measurement target from normal builds.
