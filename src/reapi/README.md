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
Scheduler mode enables Execution and Operations on the same server, dispatches the private worker
protocol, and writes an ActionCache entry only after a signed completion is accepted.
Server-side action execution uses the same fixed `SOURCE_DATE_EPOCH` and `ZERO_AR_DATE` environment
as local and wrapped actions, keeping archive timestamps outside output identity.
The scheduler client uploads its prepared native graph directly instead of issuing one large
FindMissingBlobs request; content addressing keeps repeated writes idempotent.
Normal connection closure, including the transport's lowercase EOF spelling, is not logged as a
server error.
`server.cpp` optionally wraps each accepted socket in Corosio's OpenSSL stream (ALPN `h2`, and a
required client certificate when a client CA is configured), and checks a configured bearer token on
every stream's headers before reading its body; a refusal answers gRPC status 16 and closes the
connection (liquid#415). The client maps status 16 to `errc::unauthorized`.

The implementation is private to libliquid. Message mapping, digest rules, and fixture provenance
are documented in `docs/library/reapi-codec.md`; RPCs, configuration, CLI, and peer proof are in
`docs/library/reapi.md`.
The worker protocol and scheduler capability map are documented in `docs/library/workers.md` and
`docs/api/scheduler.md`. `serve_scheduler` runs the same listener check as `serve_reapi`
(`check_listener`) before it starts its control server, and passes `scheduler_options::security`
on, so TLS and the per-RPC bearer check apply to the scheduler too (liquid#520).
