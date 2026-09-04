# Tests

The default suite covers the public library surface and the local store. Focused garbage
collection cases and its CLI flags live in `store/`; deterministic crash schedules live in `sim/`;
`test_store.cpp` covers trust-domain data-key rotation, byte-identical packs, reads across held
versions, exact-version revocation failures, and wrong-KEK atomicity. `test_repository.cpp` pins the
exact readability report for revisions written before and after rotation and its three disclosure
statements.

Secret detector, inline capture, `scan_result/1`, and redaction cases live in `scan/`.
Registry schema, freshness, commitment, and legacy digest cases live in `attestation/`.
The `formats/` cases cover
the strict JSON and `.env` codecs used by structured configuration encryption; `encryption/`
covers leaf envelopes, repository enforcement, explicit decryption, mixed-version rotation reads,
file writes, and key diffs.

Repository tests cover persisted operation records and their public newest-first view; private index
and cryptography tests stay in their existing targets. They also verify that repository collection
retains checkpointed content while reclaiming an orphan. CLI scenario and command-documentation
tests live in `cli/`. `test_runtime.cpp` repeats immediate blocking-pool completions to verify that
each continuation returns to the caller executor without losing a wake during suspension.

Build tests cover the disk action index's reopen and rebuild paths on every platform. In the
privileged Linux environment they also prove a second identical execution launches no process,
an undeclared read invalidates with both digests, unread input changes still hit, collected outputs
cannot be served, read-only publication is refused, and producer signatures verify.
