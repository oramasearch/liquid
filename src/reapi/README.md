# REAPI messages and record conversion

This directory contains plain C++ representations of the pinned Remote Execution API v2 message
set, conversion between those messages and liquid's tree, action, and result records, the Corosio
gRPC client, and the repository-backed CAS and ActionCache server. The structs depend only on
liquid's protobuf wire layer.

`DirectoryCache` retains converted subtrees by native digest, REAPI algorithm, and mode. Action
conversion uses standard fields plus platform properties for native input, toolchain, and sandbox
digests. Result conversion fills standard output and execution fields, creates version-4 native
results, and carries the canonical native record in typed auxiliary metadata so Liquid peers retain
ABI references and other fields REAPI does not model.
`action_cache.cpp` names ActionCache entries: it accepts an action digest only as lowercase
hexadecimal of the request algorithm's length, and reads and writes entries inside the cache
directory without following symbolic links, replacing an entry by rename (liquid #428).
Scheduler mode enables Execution and Operations on the same server, dispatches the private worker
protocol, and writes an ActionCache entry only after a signed completion is accepted.
Server-side action execution uses the same fixed `SOURCE_DATE_EPOCH` and `ZERO_AR_DATE` environment
as local and wrapped actions, keeping archive timestamps outside output identity.
The scheduler client uploads its prepared native graph directly instead of issuing one large
FindMissingBlobs request; content addressing keeps repeated writes idempotent.
The server reads each request stream through a bounded gRPC message reader (liquid#451): a unary
request is one message of at most 5 MiB, a ByteStream Write is applied one message at a time, and a
stream answered before it ended closes the connection. The client sends a large Write as 1 MiB
messages on one stream (`grpc_client::call_messages`).
Normal connection closure, including the transport's lowercase EOF spelling, is not logged as a
server error.

The implementation is private to libliquid. Message mapping, digest rules, and fixture provenance
are documented in `docs/library/reapi-codec.md`; RPCs, configuration, CLI, and peer proof are in
`docs/library/reapi.md`.
The worker protocol and scheduler capability map are documented in `docs/library/workers.md` and
`docs/api/scheduler.md`.
