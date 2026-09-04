# REAPI messages

This directory contains plain C++ representations of the pinned Remote Execution API v2 message
set. The structs depend only on liquid's protobuf wire layer. Storage and transport stay outside
this directory.

The implementation is private to libliquid. Its supported protocol surface and fixture provenance
are documented in `docs/library/reapi-codec.md`.
