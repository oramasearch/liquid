# Store implementation

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
