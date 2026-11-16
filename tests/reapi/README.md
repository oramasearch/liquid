# REAPI codec tests

The tests in this directory cover the dependency-free protobuf wire primitives, every committed
REAPI fixture, unknown-field retention, digest selection, record conversion, and reference
Directory Merkle roots. Conversion coverage requires version-4 native action results.
`test_action_cache.cpp` (liquid #428) checks that ActionCache entry names accept only lowercase
hexadecimal of the algorithm's length, that entries never follow a symbolic link at the entry or
the cache root, and, against a `liquid serve reapi` child process, that `UpdateActionResult` and
`GetActionResult` refuse traversal and absolute action digests without creating or overwriting a
file while a valid digest still caches. Fixture
provenance and regeneration instructions live in `fixtures/`.
