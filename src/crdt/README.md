# CRDT metadata implementation

All automerge-cpp expected values are converted to `liquid::error`: invalid local operations use
`errc::invalid_argument`, while malformed checkpoints and remote input use `errc::corrupt`.
External apply, merge, and sync receive operate on candidates and install state only after success;
the patch-returning variants observe remote transitions.
Historical document validation loads a full archive, resolves recorded heads through `get_at`,
and leaves the active shallow document unchanged. Undo batches all inverse edits for one directory
in a single wrapper transaction before publishing its candidate.

The journal stores length-prefixed canonical records. Checkpoints use `save_compact()` and replay
opens them with `load_shallow()` before applying later drips with their commit message and time.
Sync export full-loads a compact checkpoint in a temporary document so the upstream multi-round
protocol can produce a self-contained ordered message sequence. The simulated receiver starts
without its own schema change, preventing a conflicting empty root from winning Automerge's value
ordering before the sender's history arrives. Persistent repository opens remain shallow.
Redaction appends a marker instead of rewriting an earlier drip; readers resolve all markers and
expose the earlier record as redacted, while document replay skips marker records.
