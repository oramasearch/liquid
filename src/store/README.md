# Store implementation

This directory owns FastCDC chunking, append-only packs, the fixed-width index, persisted local
leases, per-kind reference extraction, and epoch collection. `collector.cpp` preserves the
ordering documented in `docs/library/gc.md`: committed tombstone manifest, index manifest, then
whole-pack removal. Partially dead pack rewriting belongs to liquid #46.
