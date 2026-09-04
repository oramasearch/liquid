# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
store. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.
