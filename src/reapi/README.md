# REAPI messages and record conversion

This directory contains plain C++ representations of the pinned Remote Execution API v2 message
set, conversion between those messages and liquid's tree, action, and result records, the Corosio
gRPC client, and the repository-backed CAS and ActionCache server. The structs depend only on
liquid's protobuf wire layer.

`DirectoryCache` retains converted subtrees by native digest, REAPI algorithm, and mode. Action
conversion uses standard fields plus platform properties for native input, toolchain, and sandbox
digests. Result conversion fills standard output and execution fields and carries the canonical
native record in typed auxiliary metadata so Liquid peers retain fields REAPI does not model.

The implementation is private to libliquid. Message mapping, digest rules, and fixture provenance
are documented in `docs/library/reapi-codec.md`; RPCs, configuration, CLI, and peer proof are in
`docs/library/reapi.md`.
