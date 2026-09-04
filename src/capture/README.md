# Capture implementation

Watcher hints are reconciled into coalesced path batches before the filter stage. Inline scanning
receives stable bytes and changed FastCDC ranges, and only accepted batches reach storage, the
metadata transaction, and the append-only journal.
