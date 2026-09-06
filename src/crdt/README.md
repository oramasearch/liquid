# CRDT metadata implementation

All automerge-cpp expected values are converted to `liquid::error`: invalid local operations use
`errc::invalid_argument`, while malformed checkpoints and remote input use `errc::corrupt`.
External apply, merge, and sync receive operate on candidates and install state only after success;
the patch-returning variants observe remote transitions.
Historical document validation loads a full archive, resolves recorded heads through `get_at`,
and leaves the active shallow document unchanged. Undo batches all inverse edits for one directory
in a single wrapper transaction before publishing its candidate.

The journal stores length-prefixed canonical records. Checkpoints prefer `save_compact()` and
immediately verify that the bytes reopen as a full sync-capable document. States that fail that
check use complete upstream history bytes. Replay applies later drips with their commit message and
time. Sync export likewise full-loads compact bytes and falls back to full bytes when required so
the upstream multi-round protocol can produce a self-contained ordered message sequence. The
simulated receiver starts without its own schema change, preventing a conflicting empty root from
winning Automerge's value ordering before the sender's history arrives. Directory enumeration
reads current tree keys after compaction, and writes retain `blob`, `tree`, or `repo` entry kinds.
Redaction appends a marker instead of rewriting an earlier drip; readers resolve all markers and
expose the earlier record as redacted, while document replay skips marker records.
