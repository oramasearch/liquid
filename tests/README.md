# Tests

Exclusive checkout coverage spans repository holder/refusal/release and offline-sync cases,
configuration lowering, connected authority, daemon fan-out, and the `cli-locks` command scenario.

The default suite covers the public library surface and the local store. Focused garbage
collection cases and its CLI flags live in `store/`; deterministic crash schedules live in `sim/`;
HTTP/2 framing, HPACK, and gRPC envelope coverage lives in `http2/`; socket-level h2c and TLS
coverage uses the private Corosio connection adapter.
`test_forge.cpp` covers lake review, comment, state, and branch-protection round trips; every
documented envelope error; bearer isolation; HTTPS with the committed certificate; rebase anchor
mapping; polling review events; and the built CLI's pinned JSON. `support/forge_double` provides the
installable in-memory server used by those cases and downstream liquify tests.
`test_deploy.cpp` covers digest-pinned control-plane deployment creation, action-key reuse,
rollback to a prior deployment or change, readiness projection, persisted deployment configuration,
and control-plane envelope failures.

Strict DER coverage rejects BER indefinite lengths, non-minimal tags, lengths, integers, and object
identifiers while preserving each element's exact encoded span for timestamp-renewal hashing.
`transparency/` records how the initial and renewed RFC 4998 fixtures were generated through lake's
production timestamp service and contains the timestamp, trust, tampering, and renewal cases.
OpenSSL generates ephemeral RSA and P-384 signatures only in the test process so the private
verification engine is checked against an independent implementation for PKCS#1 v1.5, RSA-PSS,
ECDSA, SHA-256, and SHA-384; no OpenSSL call enters the verification path.
The main test binary compiles the private signature translation unit directly so this coverage also
runs when libliquid's internal symbols are hidden by a shared build. The shared test target likewise
compiles the private package-common, tar writer and SquashFS writer translation units used by writer
byte-identity tests; those helpers remain outside the public ABI.
The CLI configuration case confirms that `log verify --timestamps` refuses to fetch when its log
URL or explicit TSA anchor file is absent.
`test_tuf.cpp` verifies Lake-produced root, targets, snapshot, and timestamp metadata; unique-key
thresholds; expiry and persisted rollback checks; exact metadata bindings; root-key recovery; full
target hashing; and the `registry trust` CLI.

Every filesystem fixture uses `temporary_directory.hpp`. The helper creates one random,
process-specific directory beneath `std::filesystem::temp_directory_path()`, honours `TMPDIR` on
Linux, resolves a relative temporary root before subprocesses can change directory, reports a clear
error when the temporary root is not writable, and removes the directory at process exit. Tests may
use fixed names beneath that directory, but must never name a fixed path directly beneath the
temporary root. `TemporaryDirectory.*` covers root selection, per-process stability, unique child
creation, cleanup, and the failure diagnostic. Subprocess daemon coverage supplies the same root
relative to the checkout before changing directory.

Scheduler protocol, duplicate-join, size-class placement, and one-requeue coverage lives in
`workers/`.
`test_data.cpp` starts a private PostgreSQL cluster when `initdb`, `pg_ctl`, and the client tools
are available. A root-run container supplies `LIQUID_TEST_POSTGRES_URL` instead. The integration
fixture prefers versioned PostgreSQL binary directories before Debian's wrapper scripts. The
case proves branch writes leave the base and sibling branch unchanged, session environments select
their own URLs through a sandboxed client action, discard removes the database, quiet-base promote
replaces it, changed-base promote is refused, records/configuration persist, and status exposes live
branches. It also proves an ordinary sandbox cannot execute an arbitrary external binary without a
data client allowance. It skips with the missing prerequisite when neither mode is available. The
portable selector case probes APFS/Linux copy-on-write support and proves an ineligible local
cluster falls back to template copying.
The shared-library macOS test target compiles the private platform launcher that its denial case
exercises, so hidden library visibility does not omit that profile coverage.
Spec parsing, canonical package records, universe loading, solver dimensions, cancellation,
explanations, optimisation, CLI output, and the generated scale workload live in `resolve/`.
Configuration-language coverage lives in `config/`: twenty exact diagnostics, typed lowering,
conditional dependencies, canonical target/source records, generator provenance, digest-verified
source fetch, and formatter idempotence.
Spack export/import coverage lives in `import/`: twenty formatter goldens, the exact 500-package
resolution and conflict workload, structural convergence cases for disjoint additions, suspicious
same-name additions and malformed proposals, explicit proposal acceptance, grammarless text
fallback, a pinned 560-package Spack export, digest-less patch pinning, and the opt-in Linux build
subset.
`test_store.cpp` covers trust-domain data-key rotation, byte-identical packs, reads across held
versions, exact-version revocation failures, wrong-KEK atomicity, pre-pack coroutine write-through
success and failure, concurrent independent writes, file-backed callback bytes, remote-registered
eviction, live-byte accounting, runtime source registration, hydration observation, collector
reclamation, and range/full rehydration. `test_repository.cpp` pins the exact readability report
for revisions written before and after rotation and its three disclosure statements.
`test_node_store.cpp` checks local and operator status, readiness, metrics, attestation bytes,
OpenAPI isolation, and the three MCP tool declarations. Its Linux FUSE case mounts two 64 MiB
images that differ by one 4 MiB file, reads both, checks shared-chunk avoidance, prefetch,
reference counts, budget eviction, and non-remote protection; macOS records the documented skip
when macFUSE is absent.

Retention coverage spans `store/`, `build/`, and `cli/`: persisted forever-ref reachability,
mixed-cost size-cap eviction, rule counts, and rule/lease/expiry explanations all run in the default
portable suite.

Secret detector, inline capture, `scan_result/1`, and redaction cases live in `scan/`.
`test_secret.cpp` covers the sealed-secret record, authenticated fields, exact scope checks,
X25519 failure paths, deterministic interoperability fixture, and Lake public-key response.
`cli/secret.cmake` exercises raw-CBOR and base64 command round trips without stderr disclosure.
Registry schema, freshness, commitment, and legacy digest cases live in `attestation/`.
Platform manifest pins remain in `test_projection.cpp`; `platform/` drives the macOS and Windows
CI installer proofs with temporary identities, native installation, program launch, and signed
sidecar verification.
`test_transparency.cpp` consumes Lake's deterministic fixture and checks checkpoint signatures,
witness quorum, every tile level, inclusion and consistency proofs, and altered or missing input.
Its `cve_2026_56865` case keeps valid leaf bytes while changing a verified parent hash tile.
`test_lake_transparency.cpp` consumes Lake's deterministic fixture and checks checkpoint signatures,
witness quorum, every full and partial tile level, inclusion and consistency proofs, cache
reauthentication, and altered or missing input. Its `cve_2026_56865` case keeps valid leaf bytes
while changing a verified parent hash tile and requires the failure to name that tile. The skipped
level case proves consistency verification completes the tile walk before requesting its proof.
`liquid-transparency-measure` times cold and cached verification of a synthetic `2^16`-leaf log.
Admission tests cover policy round trips, every image decision, TLS and insecure CLI listeners,
Kubernetes fixtures, policy-pinned checkpoint trust, valid inclusion, forged parent tiles,
untrusted checkpoints, policy reload, source readiness, metrics, REST, and MCP under `admission/`.
Delta cases create and apply a zstd reference patch, verify its registered attestation, select local
catalog records whose patch is range-hydrated through the on-disk remote source, fall back through
the same source's target chunks, and reject patch-blob or final-target digest mismatches.
The `formats/` cases cover the common registry contract, launch ZIP/tar/HDF5/Parquet/SQLite
decoders, exact Tier 1 repack, deterministic tar and SquashFS writing, volatile fields, malformed
boundaries, nesting limits, and a dynamically loaded C11 module. The strict JSON and `.env` codecs
used by
structured configuration encryption remain covered there; `encryption/`
covers leaf envelopes, repository enforcement, explicit decryption, mixed-version rotation reads,
file writes, key diffs, v1/v2/v3 wrapped-key compatibility, age specification vectors, Shamir
quorums, local and remote provider round trips, request authentication, and every ordered
cross-provider rewrap with unchanged pack bytes. The remote provider doubles bind only to loopback
and make no external requests.

Repository tests cover persisted operation records and their public newest-first view, operation
undo for writes, imported batches, records, and locks, skipped newer state, two-replica local
intent, forty-write session reversal, path-filtered session reversal, and historical document
state; private index
and cryptography tests stay in their existing targets. They also verify that repository collection
retains checkpointed content while reclaiming an orphan. The supplied-store case creates two
repository identifiers on one store, counts one physical frame for their repeated payload, collects
one repository without losing the other, and reopens by identifier. Tree-diff cases cover
additions, removals, content and mode changes, `node_id` moves, opt-in text hunks, and stored
conflict values. Text-anchor cases map unchanged and shifted lines, approximate replacements,
deleted regions and files, and a renamed node in one unsorted batch. CLI scenario and
command-documentation tests live in `cli/`. Marked guide blocks run through `guides/run.py` on
Linux, and `docs/check-links.py` checks the documentation tree on every platform. `test_runtime.cpp`
command-documentation tests live in `cli/`. Flow-control cases pin the eleventh-write refusal and
operation count, update-size rejection, cross-actor directory isolation, persisted configuration,
daemon pause/resume and bucket status, peer HTTP 429 retry projection, and the status JSON fixture.
`test_runtime.cpp`
repeats immediate blocking-pool completions to verify that
each continuation returns to the caller executor without losing a wake during suspension.
`test_stale_reasoning.cpp` covers listed and unlisted actors, persisted policy, unmoved and legacy
drip bodies, same-path line-hunk reports, declared read dependencies, disjoint and forced records,
and a synchronized replica whose signed write arrives before refusal. The CLI scenario
also covers scripted writes, default refusal, re-validation choices, and signed JSON projection.
Conflict coverage pins canonical records, text and structural producers, reopen, resolution
references, push and fresh-pull transfer, unrelated reads, build rejection, and the CRDT-suspicious
CLI test double.
Search tests compile the private index and mapped-file readers into their target, prove identical
literal and regex hit identities across scan and trigrams, pin cost choices and stale-index
exclusion, replace a changed archive's daemon-published entry documents, and parse the CLI
explanation contract. The opt-in measurement target records scan and
index costs across working-set sizes.
Every CLI CTest carries `liquid-cli`; `cli-offline` reruns that complete label through the platform
sandbox with networking denied and verifies the only four intentional network requirements.
`liquid-repository-open-measure` prepares the 100-directory, 10,000-drip checkpoint workload and
measures a fresh-process repository open for the automerge-cpp adoption comparison.
Focused CRDT cases verify commit metadata, compact checkpoint journal replay, corrupt-sync
atomicity, and upstream sync convergence; the repository replica case also covers distinct clone
actors and multi-round sync.

`test_collaboration.cpp` covers speculative agent sessions, delegated publication, semantic leases,
transient presence, epoch resynchronization, and line attribution across human and agent changes
and a path move. `test_daemon.cpp` checks that the same attribution crosses the local wire, while
`test_plugin_abi.c` is a real C plugin descriptor whose presence, lease, and attribution callbacks
are driven by `test_plugin_abi.cpp`. `attribution_measure.cpp` records the 10,000-line query cost.
`crdt/measure_epoch.cpp` records the documented compaction workload's bytes and load time.
The Unix integration case in `test_sync.cpp` exchanges two disk repositories through the public
server behind a small socket transport dispatched on the shared runtime, measures missing-object
bytes across repeated pushes, pulls into an empty repository, and rejects a corrupted TAAP
signature without changing state. It checks change-to-ref context, derives false fast-forward
state for divergent history, applies mixed per-ref outcomes, preserves exact reasons, keeps
all-refused and atomic pushes unchanged, retains uploaded objects, converts callback exceptions to
refusals, and reads a legacy reply. Metadata-only receipt verifies tree and blob records while
leaving every payload chunk remote. Its CLI case starts the real `serve sync --allow-ref main`
refusals, reads a legacy reply, and proves a flow-refused push leaves repository metadata
unchanged. Its CLI case starts the real `serve sync --allow-ref main`
process, checks ordinary and atomic per-ref output, and invokes the real push and pull commands. The
private wire codec stays in libliquid. Header compilation keeps the public sync API covered on every
platform.
The clone cases pull through an HTTP sync endpoint, force full hydration of a file above the
ordinary drive threshold, confirm the persisted remote and stable JSON, and configure and build a
small CMake project inside the checkout. A second clone resolves that checkout as a configured
drive-folder source. Separate calls pin optional destination naming, `LIQUID_TOKEN` authentication,
and the `already_exists`, `unauthorized`, and `not_found` refusal names while confirming failed
clones leave no partial target.

Drive coverage persists policy and per-path state across reopen, checks folder inheritance and
explicit overrides, retains lazy placeholders, records peer failure reasons, queues repository
writes, reports hydration state, and validates every extended-attribute value. A two-client case
edits one path differently while both clients are offline, then verifies that sequential sync keeps
both named files, the conflict references, and history for each name. The desktop badge mapping
tests run without GNOME or KDE. Linux FUSE coverage calls the mounted attribute setters, getters,
and list operation directly.

Build tests cover the disk action index's reopen and rebuild paths, public configured-plan schema,
named configuration errors, and current root digests on every platform. In the
privileged Linux environment they also prove a second identical execution launches no process,
an undeclared read invalidates with both digests, unread input changes still hit, collected outputs
cannot be served, read-only publication is refused, and producer signatures verify.
Portable query-graph tests cover early cutoff, durability-filtered walks, cooperative cancellation,
interrupted-process recovery, fingerprint-only restart validation, recorded input retrieval,
one-in-twenty clean sampling, stale-value detection, durable audit records, and eviction before
repair. `liquid-graph-measure` reports cold-start and full clean-rebuild validation over the
repository's regular files. Daemon coverage injects a stale cached query and observes its idle
validation backlog and divergence status.
Frontend tests load the Clang implementation as a separate shared object, register the same ABI
 statically, verify its sole exported entry point on macOS and Linux, and use the tree-sitter fallback
 to discover C definitions. Grammar coverage recomputes every locked parser/scanner/query digest,
parses one committed sample per configured language without error nodes, checks required tag-query
unit discovery and ABI rejection, and exercises the public grammar metadata registry. The CMake frontend fixture compiles two C++ definition objects, proves a
 body edit runs one action and one assembly, and proves a header edit reruns both recorded readers
 before assembly. Structural coverage adds handwritten move, formatting, and semantic-edit pairs
for ten languages, mechanical reindentation for every other locked sample, comments, public tier
labels, CLI text and JSON, ABI tail compatibility, and one parse per blob after a build. The
registered `frontend-one-function-measure` CTest performs the same
 one-definition edit against liquid's own tree with the configured compiler, asserts the action and
 assembly counts, and prints the elapsed milliseconds. `liquid-frontend-measure` additionally
 reports compiler-AST symbol extraction against liquid's own tree with the configured compiler.
Portable provenance cases prove the SLSA field mapping, exact manifest dependencies, tamper
rejection, and the DSSE/in-toto projection's canonical attestation digest round-trip. The real
cache-hit case also proves that the existing provenance attestation is reused without re-signing.
Portable SBOM cases prove the SPDX/CycloneDX required structure, exact input and toolchain digests,
nested-repository pins, signatures, and universal inference markers; CLI cases invoke both formats.
Portable ABI cases decode digest-pinned ELF, Mach-O, and PE fixtures, while native shared-library
fixtures exercise exported functions and objects, additions, removals, GNU symbol versions, debug
signatures, canonical records, signed compatibility attestations, and stable CLI JSON. The
privileged wrapper case checks the incompatible build event and opt-in failing exit status.
The wrap fixtures additionally exercise unmodified CMake, Autotools, recursive Make, and
`compile_commands.json` projects, a relocatable CMake install tree, and cache reuse from a different
checkout path. Portable relocation cases scan clean and embedded prefixes, absolute package
metadata, and a digest-pinned Mach-O with an absolute install name. Reproducibility
cases cover signed agreement and disagreement, random-byte detection, permanent cache quarantine,
rate-one sampling, and CLI verification of every wrapped action. The same wrap fixture performs a
complete graph pass and requires zero query divergences.
Portable build
diagnostic cases cover the implemented miss categories, imported action history, cumulative byte
statistics, recorded scheduling decisions and inputs, and the JSON forms of `why` and `cache stats`.
They also cover canonical output normalisation, signed input/output claims, named dependency-tree
folding, cross-project cascade termination, and an opt-in pinned zlib/libpng/program chain that
records process counts and load average.
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

`consumer/` is an independent CMake project that finds the installed liquid package, then builds
and runs an executable linked to `liquid::liquid`. `consumer-project-build` installs the already-
built parent artifacts into a test-local prefix, configures the consumer with the parent generator
and C++ compiler, and compiles only the consumer source. `cli-identity-codec` creates and rotates an
identity through the CLI, then decodes, re-encodes, and verifies the stored canonical-CBOR files
through the public API. `test_hardware_keys.cpp` drives CTAP2, PIV, and TPM command-level doubles,
cross-backend identity rotation, malformed signature rejection, and threshold release approval
including compromise-time refusal. On Linux it also drives an installed `swtpm` through the TPM
command transport.

Symbol tests verify compiler-labelled definition/reference records, cross-file identifier matching,
repository storage round trip, the `liquid symbols` command, and the labelled tree-sitter fallback.
Hot-update tests classify fixture pairs across all tiers, prove that comment-only output equality is
tier 0, require ABI evidence for tier 2, reject unsigned patches, and reconstruct a two-patch
composition. The Unix demo drives one Capy process across a signed tier-1 replacement; its tier-3
case verifies the sandboxed restart-hook arguments where a FUSE backend is installed.
`test_projection.cpp` imports the committed `fixtures/projection/root` install tree and covers
package round trips, registry recognition, deterministic compression and output, OCI layers and
attestation referrers, signed `projection/1`, digest-only Helm values, Homebrew bottles, Arch
recipes, AppImage, Flatpak, Snap, adjacent DSSE collections, relocation refusal, and native Helm
lint when the tool is installed. Debian source coverage pins the control, rules, changelog,
copyright, and `.dsc` digest, and emits the peer fixture for `sbuild`, `lintian`, and `debuild`.
It round-trips the owned SquashFS through `unsquashfs` when that tool is installed. The pinned
target digests are the macOS/Linux byte-identity contract.
attestation referrers, signed `projection/1`, digest-only Helm values, and native Helm lint when
the tool is installed. It also pins twelve generated lifecycle scripts, quoting/refusal behavior,
script attestation and SBOM binding, batch and installed-inventory ownership conflicts, and derived
replacements. `test_install.cpp` covers intent application, generation listing and rollback, and a
subprocess `SIGKILL` after every staged file and at the selection switch. The pinned target digests
are the macOS/Linux byte-identity contract.
the tool is installed. It also pins a GLib-derived OSTree commit checksum and bootc manifest digest,
exercises base and overlay paths, writes `archive-z2` and `bare-user`, and checks symbolic-link
objects. The pinned target digests are the macOS/Linux byte-identity contract.
The format-registry inventory also pins the Debian, RPM, and Alpine codecs beside the five launch
codecs.
Support-matrix tests compare every live codec and locked grammar with `support_matrix()`, and pin
the compiler frontend as the strongest C++ registration.
