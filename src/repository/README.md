# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
store. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.

The build adapter uses the repository's device signer for result records and reproducibility
attestations, reads the persisted `cache.publish` and `verify.rate` policy, retains the action-index
root with a repository lease, and owns per-handle cache counters and critical alerts exposed by
`slsa_provenance/1` uses the same device signer. It reads the persisted `cache.publish` and
`verify.rate` policy, retains the action-index root with a repository lease, and owns per-handle
cache counters and critical alerts exposed by
`repository::cache_stats`.
