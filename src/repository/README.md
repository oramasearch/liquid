# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
store. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.

The build adapter uses the repository's device signer for result records, reads the persisted
`cache.publish` policy, retains the action-index root with a repository lease, and owns per-handle
cache hit, miss, and byte counters exposed by `repository::cache_stats`.
