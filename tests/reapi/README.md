# REAPI codec tests

The tests in this directory cover the dependency-free protobuf wire primitives, every committed
REAPI fixture, unknown-field retention, digest selection, record conversion, and reference
Directory Merkle roots. Conversion coverage requires version-4 native action results. Fixture
provenance and regeneration instructions live in `fixtures/`.

`test_server_auth.cpp` starts the real `liquid serve reapi --token-file` on loopback and proves that
`UpdateActionResult`, `BatchUpdateBlobs` and `GetCapabilities` without the token, or with a wrong
one, are refused and leave no ActionCache entry, while the same write with the token is stored
(liquid#415). It also starts the real `liquid scheduler --token-file` and proves the same refusals
for the scheduler's REAPI listener (liquid#520). It runs on Unix static builds that build the CLI.
