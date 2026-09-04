# Secret scanning tests

These tests cover matching and harmless inputs for every detector plus repository path rules and
the literal line marker. Test names and assertion output never contain matched values.

Capture tests also hold a blocked file across a periodic reconciliation, ensuring a delayed watcher
hint cannot deliver the same safe block twice.
