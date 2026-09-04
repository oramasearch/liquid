# CRDT metadata implementation

The journal stores length-prefixed canonical records. Redaction appends a marker instead of
rewriting an earlier drip; readers resolve all markers and expose the earlier record as redacted,
while document replay skips marker records.
