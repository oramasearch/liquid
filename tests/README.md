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
`liquid-repository-open-measure` prepares the 100-directory, 10,000-drip checkpoint workload and
measures a fresh-process repository open for the automerge-cpp adoption comparison.
Focused CRDT cases verify commit metadata, compact checkpoint journal replay, corrupt-sync
atomicity, and upstream sync convergence; the repository replica case also covers distinct clone
actors and multi-round sync.

Build tests cover the disk action index's reopen and rebuild paths on every platform. In the
privileged Linux environment they also prove a second identical execution launches no process,
an undeclared read invalidates with both digests, unread input changes still hit, collected outputs
cannot be served, read-only publication is refused, and producer signatures verify.
Portable provenance cases prove the SLSA field mapping, exact manifest dependencies, tamper
rejection, and the DSSE/in-toto projection's canonical attestation digest round-trip. The real
cache-hit case also proves that the existing provenance attestation is reused without re-signing.
The wrap fixtures additionally exercise unmodified CMake, Autotools, recursive Make, and
`compile_commands.json` projects and cache reuse from a different checkout path. Reproducibility
cases cover signed agreement and disagreement, random-byte detection, permanent cache quarantine,
rate-one sampling, and CLI verification of every wrapped action.
Portable build
diagnostic cases cover the implemented miss categories, imported action history, cumulative byte
statistics, and the JSON forms of `why` and `cache stats`.
Mount-backed observability coverage constructs a two-action sleeping dependency chain and verifies
result timing, durable trace edges, critical-path queries, and Chrome trace-event JSON.
