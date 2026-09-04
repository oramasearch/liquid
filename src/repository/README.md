# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
store. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.

The build adapter uses the repository's device signer for result records and reproducibility
attestations, reads the persisted `cache.publish` and `verify.rate` policy, retains the action-index
root with a repository lease, and owns per-handle cache counters and critical alerts exposed by
`repository::cache_stats`.

For build diagnostics, the same adapter resolves a path's current content to the newest matching
write operation and signed drip. Content, node, actor, and timestamp must all agree; otherwise the
caller receives no attribution.
