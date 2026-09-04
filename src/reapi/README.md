# REAPI messages and record conversion

This directory contains plain C++ representations of the pinned Remote Execution API v2 message
set and the conversion between those messages and liquid's tree, action, and result records. The
structs depend only on liquid's protobuf wire layer. Storage and transport stay outside this
directory; callers supply already-loaded object bytes to the tree and result converters.

`DirectoryCache` retains converted subtrees by native digest, REAPI algorithm, and mode. Action
conversion uses standard fields plus platform properties for native input, toolchain, and sandbox
digests. Result conversion fills standard output and execution fields and carries the canonical
native record in typed auxiliary metadata so Liquid peers retain fields REAPI does not model.

The implementation is private to libliquid. Its supported protocol surface, mapping, digest rules,
and fixture provenance are documented in `docs/library/reapi-codec.md`.
