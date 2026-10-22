# Secret scanning tests

These tests cover matching and harmless inputs for every detector plus repository path rules and
the literal line marker. Test names and assertion output never contain matched values.

Capture tests also hold a blocked file across a periodic reconciliation, ensuring a delayed watcher
hint cannot deliver the same safe block twice.

Repository and capture fixtures allocate their named directories beneath the unique process root
specified in the parent [`README.md`](../README.md).

JWT regressions cover long invalid components with many candidate prefixes under a two-second
budget, valid candidates inside later components and after failed candidates, minimum component
lengths, valid components above 64 KiB, and detection beyond a two-MiB prefix without skipping
other detectors.
