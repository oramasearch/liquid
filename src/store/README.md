# Store implementation

Repository configuration encryption uses the private `repository_detail::store_access` adapter to
choose a plaintext metadata put or an explicit trust-domain encrypted put for whole-file payload
chunks; the public store API keeps its existing default-key behavior.

This directory owns FastCDC chunking, append-only packs, the fixed-width index, persisted local
leases, per-kind reference extraction, and epoch collection. `collector.cpp` preserves the
ordering documented in `docs/library/gc.md`: committed tombstone manifest, index manifest, then
whole-pack removal. `compactor.cpp` rewrites selected partially dead packs by copying physical
frames byte-for-byte, with the collection manifest rename as the commit point. The object map also
provides `store::key_version` with the recorded trust-domain data-key version and verifies whether
the attached keyring can currently unwrap it; key-id parsing stays in `src/identity/key_version.hpp`.

Targeted redaction removes selected object-map entries, commits tombstones for their unshared
frames, rebuilds the index, and compacts only affected packs. It is separate from lease-driven
reachability collection so unrelated repository objects remain available.

Build-cache collection roots are canonical `action-index-root` records under the same store. The
reference registry traverses root to result, result to action/output/stdout/stderr, and action to
input/toolchain/sandbox. It also traverses root to the verification attestation, so an indexed
result, its trust evidence, and the bytes needed to use it remain one reachable graph.
