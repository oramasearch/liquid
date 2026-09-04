# CRDT metadata implementation

All automerge-cpp expected values are converted to `liquid::error`: invalid local operations use
`errc::invalid_argument`, while malformed checkpoints and remote input use `errc::corrupt`.
External apply, merge, and sync receive operate on candidates and install state only after success;
the patch-returning variants observe remote transitions.

The journal stores length-prefixed canonical records. Checkpoints use `save_compact()` and replay
opens them with `load_shallow()` before applying later drips with their commit message and time.
Redaction appends a marker instead of rewriting an earlier drip; readers resolve all markers and
expose the earlier record as redacted, while document replay skips marker records.
