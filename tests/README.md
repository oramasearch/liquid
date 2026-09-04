# Tests

The default suite covers the public library surface and the local store. Focused garbage
collection cases and its CLI flags live in `store/`; deterministic crash schedules live in `sim/`;
private index and cryptography tests stay in their existing targets.

`test_store.cpp` covers trust-domain data-key rotation, byte-identical packs, reads across held
versions, exact-version revocation failures, and wrong-KEK atomicity. `test_repository.cpp` pins the
exact readability report for revisions written before and after rotation and its three disclosure
statements.

Secret detector, inline capture, attestation, and redaction cases live in `scan/`.
