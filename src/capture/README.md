# Capture implementation

Watcher hints are reconciled into coalesced path batches before the filter stage. Inline scanning
receives stable bytes and changed FastCDC ranges, and only accepted batches reach storage, the
metadata transaction, and the append-only journal.

A blocked scan still saves the reconciled filesystem snapshot. It neither reaches storage nor
updates the accepted chunk list, but identical bytes from a delayed watcher hint are not reported
again.
