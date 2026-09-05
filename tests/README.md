# Tests

The default suite covers the public library surface and the local store. Focused garbage
collection cases and its CLI flags live in `store/`; deterministic crash schedules live in `sim/`;
HTTP/2 framing, HPACK, and gRPC envelope coverage lives in `http2/`; socket-level h2c and TLS
coverage uses the private Corosio connection adapter.
Scheduler protocol, duplicate-join, size-class placement, and one-requeue coverage lives in
`workers/`.
`test_store.cpp` covers trust-domain data-key rotation, byte-identical packs, reads across held
versions, exact-version revocation failures, and wrong-KEK atomicity. `test_repository.cpp` pins the
exact readability report for revisions written before and after rotation and its three disclosure
statements.

Retention coverage spans `store/`, `build/`, and `cli/`: persisted forever-ref reachability,
mixed-cost size-cap eviction, rule counts, and rule/lease/expiry explanations all run in the default
portable suite.

Secret detector, inline capture, `scan_result/1`, and redaction cases live in `scan/`.
Registry schema, freshness, commitment, and legacy digest cases live in `attestation/`.
Delta cases create and apply a zstd reference patch, verify its registered attestation, select local
catalog records whose patch is range-hydrated through the on-disk remote source, fall back through
the same source's target chunks, and reject patch-blob or final-target digest mismatches.
The `formats/` cases cover
the strict JSON and `.env` codecs used by structured configuration encryption; `encryption/`
covers leaf envelopes, repository enforcement, explicit decryption, mixed-version rotation reads,
file writes, and key diffs.

Repository tests cover persisted operation records and their public newest-first view; private index
and cryptography tests stay in their existing targets. They also verify that repository collection
retains checkpointed content while reclaiming an orphan. The supplied-store case creates two
repository identifiers on one store, counts one physical frame for their repeated payload, collects
one repository without losing the other, and reopens by identifier. Tree-diff cases cover
additions, removals, content and mode changes, `node_id` moves, opt-in text hunks, and stored
conflict values. Text-anchor cases map unchanged and shifted lines, approximate replacements,
deleted regions and files, and a renamed node in one unsorted batch. CLI scenario and
command-documentation tests live in `cli/`. `test_runtime.cpp`
repeats immediate blocking-pool completions to verify that
each continuation returns to the caller executor without losing a wake during suspension.
`liquid-repository-open-measure` prepares the 100-directory, 10,000-drip checkpoint workload and
measures a fresh-process repository open for the automerge-cpp adoption comparison.
Focused CRDT cases verify commit metadata, compact checkpoint journal replay, corrupt-sync
atomicity, and upstream sync convergence; the repository replica case also covers distinct clone
actors and multi-round sync.
The Unix integration case in `test_sync.cpp` exchanges two disk repositories through the public
server behind a small socket transport dispatched on the shared runtime, measures missing-object
bytes across repeated pushes, pulls into an empty repository, and rejects both a corrupted TAAP
signature and authorization callbacks for changes or refs without changing state. It checks
change-to-ref context and derives false fast-forward state for divergent history. Its CLI case
starts the real `serve sync --allow-ref main` process, accepts `main`, rejects a disallowed ref,
and invokes the real push and pull commands. The private wire codec stays in libliquid. Header
compilation keeps the public sync API covered on every platform.

Build tests cover the disk action index's reopen and rebuild paths on every platform. In the
privileged Linux environment they also prove a second identical execution launches no process,
an undeclared read invalidates with both digests, unread input changes still hit, collected outputs
cannot be served, read-only publication is refused, and producer signatures verify.
Portable query-graph tests cover early cutoff, durability-filtered walks, cooperative cancellation,
interrupted-process recovery, fingerprint-only restart validation, and recorded input retrieval.
`liquid-graph-measure` reports cold-start validation over the repository's regular files.
Frontend tests load the Clang implementation as a separate shared object, register the same ABI
statically, verify its sole exported entry point on macOS and Linux, and use the tree-sitter fallback
to discover C definitions. The CMake frontend fixture compiles two C++ definition objects, proves a
body edit runs one action and one assembly, and proves a header edit reruns both recorded readers
before assembly. The registered `frontend-one-function-measure` CTest performs the same
one-definition edit against liquid's own tree with the configured compiler, asserts the action and
assembly counts, and prints the elapsed milliseconds.
Portable provenance cases prove the SLSA field mapping, exact manifest dependencies, tamper
rejection, and the DSSE/in-toto projection's canonical attestation digest round-trip. The real
cache-hit case also proves that the existing provenance attestation is reused without re-signing.
Portable SBOM cases prove the SPDX/CycloneDX required structure, exact input and toolchain digests,
nested-repository pins, signatures, and universal inference markers; CLI cases invoke both formats.
The wrap fixtures additionally exercise unmodified CMake, Autotools, recursive Make, and
`compile_commands.json` projects and cache reuse from a different checkout path. Reproducibility
cases cover signed agreement and disagreement, random-byte detection, permanent cache quarantine,
rate-one sampling, and CLI verification of every wrapped action.
Portable build
diagnostic cases cover the implemented miss categories, imported action history, cumulative byte
statistics, recorded scheduling decisions and inputs, and the JSON forms of `why` and `cache stats`.
Mount-backed observability coverage constructs a two-action sleeping dependency chain and verifies
result timing, durable trace edges, critical-path queries, scheduling metadata, and Chrome
trace-event JSON. Distribution coverage uses a second store to materialise source and toolchain
trees, compares remote and local output, measures a mixed cost-model workload, batches short work,
and verifies that racing cancels its loser and publishes one result.
Input-policy cases cover refusal and warning reports, verified approved identities, invalid
signatures, registered profiles, independent-builder quorums, toolchain inputs, subject-index
recovery, and cache isolation after a policy change.

Remote-output coverage uses an on-disk counting source to distinguish registration from transfer,
prove chunk-exact hydration and materialisation, and exercise local/remote/absent store status.
The privileged Linux build suite also proves eligible actions reject and name a host-path read.

`reapi/` covers the dependency-free protobuf wire forms, the pinned REAPI message fixtures,
unknown-field retention, hash selection, and liquid record conversion. The codec stays private to
libliquid, so this target compiles its private sources directly for static and shared builds.

`consumer/` is an independent CMake project that adds the pinned Capy and Corosio checkouts plus
nlohmann/json before liquid, then builds and runs an executable linked to `liquid::liquid`.
`consumer-project-build` configures and builds it as one CTest step with the parent build's compiler
and static/shared setting. `cli-identity-codec` creates and rotates an identity through the CLI,
then decodes, re-encodes, and verifies the stored canonical-CBOR files through public libliquid API.
