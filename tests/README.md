# Tests

The default suite covers the public library surface and the local store. Focused garbage
collection cases and its CLI flags live in `store/`; deterministic crash schedules live in `sim/`;
`test_store.cpp` covers trust-domain data-key rotation, byte-identical packs, reads across held
versions, exact-version revocation failures, and wrong-KEK atomicity. `test_repository.cpp` pins the
exact readability report for revisions written before and after rotation and its three disclosure
statements.

Secret detector, inline capture, attestation, and redaction cases live in `scan/`.
The `formats/` cases cover
the strict JSON and `.env` codecs used by structured configuration encryption; `encryption/`
covers leaf envelopes, repository enforcement, explicit decryption, mixed-version rotation reads,
file writes, and key diffs.

Repository tests cover persisted operation records and their public newest-first view; private index
and cryptography tests stay in their existing targets. CLI scenario and command-documentation tests
live in `cli/`.
