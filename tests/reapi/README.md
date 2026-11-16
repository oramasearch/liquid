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

`test_request_limits.cpp` starts `liquid serve reapi` and checks the request limits of liquid#451:
a unary request or a Write message whose prefix declares more than 5 MiB is answered with
`RESOURCE_EXHAUSTED` before its body is sent, a 7 MiB Write sent as 1 MiB messages on one stream
is stored, and `grpc_client::call_messages` uploads a blob above the limit.
`test_uploads.cpp` covers ByteStream limits, capacity accounting through completion, empty-upload
quotas, ownership and cancellation cleanup, expiry without status polling refreshing the timeout,
and resumption. Its loopback server tests exercise limits across separate RPC connections, idle
expiry, malformed or compressed later messages, resource changes, finishing-message ordering and
a zero HTTP/2 response window that must time out and release its active upload.
The tests use small configured budgets and deterministic clock values for unit checks.
`test_server_auth.cpp` starts the real `liquid serve reapi --token-file` on loopback and proves that
`UpdateActionResult`, `BatchUpdateBlobs` and `GetCapabilities` without the token, or with a wrong
one, are refused and leave no ActionCache entry, while the same write with the token is stored
(liquid#415). It also starts the real `liquid scheduler --token-file` and proves the same refusals
for the scheduler's REAPI listener (liquid#520). It runs on Unix static builds that build the CLI.
