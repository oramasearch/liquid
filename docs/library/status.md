# Implementation status

This page states what libliquid and the `liquid` binary implement today, by phase, and what each
phase still lacks. It is the consolidated status record that the root `README.md` used to carry;
the root file now keeps a short summary and links here. Each paragraph names the page that holds
the design and verification detail. When a change makes a sentence here false, correct it in the
same commit.

**Code shape.** Code is a C++26 **libliquid** (`liquid::liquid`, static by default, shared with
`BUILD_SHARED_LIBS`) plus the `liquid` binary as a thin client of it; liquify and lake link the
same library. The public waiting operations are lazy Boost.Capy tasks, backed by a private Corosio
runtime ([`runtime.md`](runtime.md)). Issue #285 completes the installed static dependency
closure: `find_package(liquid CONFIG REQUIRED)` and `liquid::liquid` support repository
create/open, persisted file write/read, and consumer-defined coroutines, checked after moving
the install prefix
([`libliquid.md`](libliquid.md)). The `liquid` binary is the documented thin client of the Phase 1
repository, capture, VFS, store, and identity APIs plus the Phase 2 build wrapper
([`../cli/README.md`](../cli/README.md)).

## Phase 1: the store

**The Phase 1 store exit criterion is met.** The chunk store, bounded-memory file streaming,
garbage collection with pack compaction, deterministic crash simulator, first working repository,
local Git import/export, pluggable KEK custody with trust-domain data-key rotation and revision
readability, TAAP/1 software, FIDO2, and PIV identities with cross-backend rotation plus signed
drips and changes, TPM PCR-bound sealing, signed threshold release approval, local change capture,
agent write flow control, inline secret scanning, verified redaction, structured JSON/`.env`
encryption, Lake sealed-secret envelopes, and the macOS/Linux checkout daemon are implemented.

**Public API.** The public error, digest, identity document codec, signature decoder, and release
approval APIs are implemented, including BLAKE3, SHA-256, and keyed BLAKE3 identifiers
([`api-map.md`](api-map.md), [`hash-agility.md`](hash-agility.md), [`identity.md`](identity.md),
[`release-approval.md`](release-approval.md)).

**Store.** The store implements an injected coroutine-native persistence backend with disk and
in-memory reference implementations, plus open, put, bounded-memory `put_file` and `get_to_file`, get,
range read, contains, trust-domain encrypted objects with passphrase, raw-key-file, age/X25519,
cloud KMS, lake transit, Shamir, or TPM PCR-bound custody, versioned data-key rotation and
revocation, repository readability reports, leased roots, epoch garbage collection, and pack compaction
([`store.md`](store.md), [`encryption.md`](encryption.md), [`gc.md`](gc.md),
[`simulation.md`](simulation.md)). The disk backend remains the default, and the crash simulator
runs the same invariants against disk and memory. Store owners can await a whole-content write-through callback
before the first local pack write and evict remote-registered local copies without losing their
remote location. Multiple repositories can use one caller-owned store with repository-scoped
checkpoints, journals, and retention leases. `liquid gc --explain` reports why retained chunks are
live.

**Repository.** Repository creation, working-tree reads and writes, recorded moves and removals,
change cuts, references, snapshots, historical document state, composition, conflict values,
operation- and session-scoped local-intent undo, advisory locks, private speculative agent
sessions, semantic file/symbol/range leases, transient presence, compaction epochs, scoped
delegation attribution, stable-node per-line attribution, status, public tree/change diff,
edit-surviving text anchors, paged result,
attestation, trace, and SBOM enumeration, one-drip streaming file writes and tree imports, and
caller-stated write bases with stale-reasoning refusal, disjoint re-validation, and forced
re-planning records are implemented ([`object-model.md`](object-model.md),
[`multiplayer.md`](multiplayer.md)). TAAP/1 identities with
rotation and signed drips and changes, the internal CRDT metadata document, canonical CBOR codec,
and drip journal are implemented behind libliquid. Directory CRDT checkpoints prefer compact bytes
with checked full-history fallback and reopen as sync-capable documents; remote changes are
observed through patches, and every automerge apply, merge, and sync-receive failure is checked.

**Capture, scanning, and attestations.** Capture subscriptions, default-blocking secret scanning,
the TAAP attestation profile registry with `scan_result/1`, `reproducibility/1`, structured
`slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, `image_provenance/1`, `release_policy/1`,
and `release_approval/1`, and repository redaction are implemented
([`capture.md`](capture.md), [`secret-scanning.md`](secret-scanning.md),
[`attestations.md`](attestations.md), [`release-approval.md`](release-approval.md)).

**Encryption of structured values.** Repository path rules encrypt selected JSON and `.env` leaf
values while leaving their structure visible, with explicit plaintext reads and key-only
historical diff. Whole-file encryption remains the fallback
([`leaf-encryption.md`](leaf-encryption.md), [`sealed-secrets.md`](sealed-secrets.md)).

**Git.** `liquid import git` preserves reachable local Git history and its identity mapping;
`liquid export git` writes a standalone Git repository and reports compatibility losses
([`git-interop.md`](git-interop.md)).

**Sync and review.** Repository push and pull, signed metadata exchange, missing-object
negotiation, the public sync server with change and graph-derived per-reference authorization,
mixed and atomic push outcomes, and the shared HTTP/1.1 client, `serve sync` adapter, and lake
forge review client with its polling event stream and installable double are implemented
([`sync.md`](sync.md), [`forge-client.md`](forge-client.md)).

**Drive.** Issue #195 adds the persisted three-tree drive client, named concurrent conflict values,
interrupted-transfer resume, per-reference refusal reporting, stable-node path history and restore,
`liquid drive`, and the daemon drive target. Issue #258 binds imported active directory documents
to each destination repository's actor, so two clients retain both named histories after concurrent
offline edits. Issue #235 adds persisted online-only,
available-offline, and excluded path state; pin and unpin commands; xattrs; policy-driven and
offline-aware hydration; queued-write and hydration-debt status; and GNOME Files and Dolphin badge
providers. Issue #247 adds `liquid clone` over the same client, including first-pull identity
adoption, persisted remote configuration, full plain-checkout materialization, and stable JSON
output ([`drive.md`](drive.md), [`../cli/README.md`](../cli/README.md)).

**VFS.** VFS full materialisation works on every platform; lazy mounts use libfuse3 on Linux and
optional macFUSE on macOS ([`vfs.md`](vfs.md)).

**Transparency and trust.** Repository metadata retains verified Lake transparency checkpoints:
the public client verifies signed witness-quorum checkpoints, all entry and parent hash tiles,
exact TAAP inclusion, consistency, and identity scans before callers trust a log response. Each
full or partial tile is reauthenticated through its parents to the selected checkpoint; cached
tile bytes receive the same complete walk, and consistency verification completes that walk before
it requests a proof.
Complete transparency inclusion verification checks signed checkpoints, witness quorum, tiles, and
proofs before log mode accepts a record. Strict DER, CMS, and X.509 parsing plus verification-only
RSA and ECDSA validate RFC 3161 checkpoint timestamps and RFC 4998 renewal chains against
explicitly pinned TSA trust anchors at each token's historical generation time
([`transparency.md`](transparency.md)). The repository-backed TUF client verifies Lake root
rotation, timestamp, snapshot, targets, and complete target bytes before registry or drive clients
may accept them; `liquid registry trust` pins or displays the out-of-band root
([`tuf.md`](tuf.md)).

**Admission.** `liquid serve admission` provides the fail-closed Kubernetes webhook, TLS and
control listeners, atomically reloaded named policy sets at `/admit/<policy>`, structured
CBOR/JSON decisions in AdmissionReview status, a bounded local decision stream, local or
node-store evidence lookup, and policy-pinned transparency verification through the shared
checkpoint, parent-tile, and inclusion verifier. Each decision records the log outcome and exact
failure reason. Metrics, REST, OpenAPI, and MCP expose the same record
([`admission.md`](admission.md),
[`../api/admission.md`](../api/admission.md)).

**Search.** Repository-local BM25F, substring, and regex search over the working tree or named refs
is implemented with measured cost planning, current mapped scans, stable explanations, and atomic
per-drip publication ([`search.md`](search.md)).

**CLI and documentation.** The CLI validates every option from one typed command table and
generates its Markdown, stable JSON, and installed man-page reference from that table
([`../reference/README.md`](../reference/README.md)). Executable guides cover an existing wrapped
build, a first repository and sync peer, and the current keys-and-encryption boundary
([`../guides/README.md`](../guides/README.md)). CI renders the complete documentation tree as a
downloadable static-site artifact ([`../build/ci.md`](../build/ci.md)).

## Phase 2: the build engine

**Phase 2 includes** `liquid wrap`, local sandboxed action execution, the persistent disk cache,
sampled reproducibility verification, signed SLSA provenance, record-derived cache diagnostics,
dependency-aware build observability, build-input policy, a cross-project configure fact base,
cost-aware retention, content-addressed upstream outputs, graph-derived SPDX and CycloneDX SBOMs
for every output, and stored ABI descriptors plus signed compatibility comparisons for every built
shared library.

**Actions and the wrapper.** `liquid::build` stores action and result records, materialises a
toolchain, observes reads through the VFS mount, and executes the action in Linux namespaces or a
macOS sandbox with a fixed environment ([`build-engine.md`](build-engine.md),
[`../build/sandboxed-executor.md`](../build/sandboxed-executor.md)). `liquid wrap` runs an
unchanged CMake, Autotools, Make, or compilation-database project in one observed mount and turns
output-producing tool processes into cached actions. Before execution it looks up signed results by
candidate key, validates every observed manifest path against the current input tree, and reports
the first differing input on a miss. The on-disk action index rebuilds from its leased store root.

**Verification.** `liquid verify` re-executes cached actions in fresh sandboxes, compares exit
status and every output digest, signs a `reproducibility/1` attestation, and permanently
quarantines a divergent result. `wrap` schedules one-in-20 graph and action samples on the shared
runtime by default.

**Outputs and inputs.** New signed results carry canonical output-tree digests.
`wrap --input <name>=<result|tree>` and direct named inputs mount upstream bytes read-only below
`deps/<name>/`; an identical normalised upstream tree stops the downstream cascade. Directory
modes, `ar` headers, and Python bytecode are normalised before storage, with the rule set bound
into the sandbox digest. `wrap --install-prefix` stages a canonical `bin/lib/include/share` install
tree with relative ELF or Mach-O lookup paths. Publication rejects embedded sandbox or install
prefixes and absolute binary or package-metadata paths; an explicit waiver is retained in result
diagnostics ([`relocation.md`](relocation.md)).

**Provenance, SBOM, and ABI.** `liquid provenance` verifies the result-bound TAAP attestation and
`--dsse` projects it as an in-toto Statement ([`provenance.md`](provenance.md)). `liquid sbom`
verifies and prints SPDX 2.3 or CycloneDX 1.5; foreign binaries use distinctly marked local
composition analysis ([`sbom.md`](sbom.md)). Stored ABI descriptors and signed compatibility
comparisons cover every built shared library ([`abi.md`](abi.md)).

**Diagnostics and observability.** `liquid why` explains output, action, and result misses with
digest, argv, environment, toolchain, sandbox, line-range, and drip details; `wrap --explain` and
`cache stats` report disk and remote hit rates and output bytes avoided versus rebuilt. Each result
records non-overlapping queued, sandbox, exec, hash, and store phases. Every wrap retains a
dependency trace, prints wall and critical-path time plus achieved parallelism and phase totals,
and supports `liquid build trace --chrome` and `liquid why-slow <target>`.

**Policy, facts, and retention.** Repository `config.policy` rules can require registered-profile
attestations from approved builders for source and toolchain inputs before execution;
`liquid policy check` evaluates a cached target without building, and the policy digest isolates
cache candidates ([`policy.md`](policy.md)). Autotools cache variables and common CMake check
results are facts keyed by compiler content, explicit sysroot content, and target triple;
`liquid facts` inspects or exports them. `wrap --target` refuses to execute an uncached probe for a
foreign target. Typed retention rules keep matching refs and cost-weighted results through computed
reachability; a physical size cap evicts the lowest execution-cost-per-byte results, and
`liquid retain explain` names the responsible rule and lease ([`gc.md`](gc.md)).

**Remote execution.** Issue #18 adds a transport-independent remote executor alongside #17's remote
blob interface, ships source and toolchain trees from the store, and chooses local, remote, or
racing execution from recorded timing, absent bytes, queue depth, and cache locality. Four or more
short actions share one supported remote batch; a race cancels its loser and publishes only its
winner. Results, `liquid why`, retained traces, and Chrome events preserve the decision and every
model input ([`distribution.md`](distribution.md)). The dependency-free protobuf codec, REAPI v2.3
messages, and native tree/action/result conversions are implemented
([`reapi-codec.md`](reapi-codec.md)). The Corosio REAPI client provides CAS, ByteStream,
ActionCache, and Execution RPCs; `wrap` and `build --profile` use its validated remote cache tier,
and `liquid serve reapi` exposes liquid's CAS and ActionCache ([`reapi.md`](reapi.md)). Liquid's
action-digest scheduler and TAAP-signed worker fleet are implemented, including one bounded requeue
after worker loss, size-class placement from #18's recorded decision, REAPI Execution and
ActionCache, and deployable health, metrics, REST, OpenAPI, and MCP surfaces
([`workers.md`](workers.md)).

**Node store.** The per-node runtime chunk store is implemented with metadata-only image receipt,
reference-counted read-only VFS mounts, shared-chunk reuse counters, logical byte-budget eviction,
a local snapshotter listener, TCP health/metrics/REST/OpenAPI/MCP control, and the
`liquid node-store` CLI ([`node-store.md`](node-store.md)).

**liquid builds liquid.** Linux CI configures FetchContent dependencies outside the sandbox, builds
the static library and CLI under `liquid wrap` twice, requires a 100% disk hit rate on the second
run, and compares both outputs byte for byte with the CMake bootstrap. Static and shared libliquid
builds remain covered ([`self-host.md`](self-host.md)).

**Phase 2 limits.** The action-result memory cache tier remains separate work. macOS needs macFUSE
and Windows returns its documented unsupported result. Measured transfer timing remains Phase 3
work. The containerd proxy snapshotter remains in k0s #14.

## Phase 3: distribution

**Build without bytes** is implemented behind a transport-neutral remote blob source. Remote result
records retain source and instance locations without fetching outputs. Store reads, VFS hydration,
and materialisation fetch verified logical chunks only on demand, while `liquid status` reports
current-tree bytes that remain remote. Actions carry an opt-in remote-eligibility flag, default
off; on Linux, eligible actions refuse and name reads outside their input and toolchain trees
before dispatch. Cost-based scheduling, the REAPI record codec, the Corosio client, and the
validated read-only remote cache tier are implemented. NativeLink and BuildBarn match the local
100% second-run fixture hit rate, and stock Bazel reads and writes `liquid serve reapi`
([`distribution.md`](distribution.md), [`reapi.md`](reapi.md)).

**Explicit local replay.** `liquid replay <action|result>` reconstructs a recorded action from its
input tree, toolchain, environment, and sandbox, then compares exit status, stdout, stderr, and
declared outputs byte for byte. Matching recorded failures are successful reproductions, and
differences report the first unequal byte. Replay does not publish its local result or change
quarantine state unless `--record` requests an ordinary verification sample. Registered remote
inputs and expected outputs hydrate through their recorded `remote_blob_location` and the
repository's configured `remote_blob_source`; tests cover both its on-disk double and the REAPI
client.

**Delta delivery.** `liquid delta create` stores a zstd reference patch, content-addressed delta
record, and signed `delta/1` attestation. Apply and fetch verify source, patch-blob, and final
target digests. Fetch finds the local signed target catalog and uses the store's registered remote
blobs when the patch or target remains remote, otherwise falling back to content-defined chunks.
The executable-aware algorithm id is reserved because its measured Mach-O candidate was larger
than the generic patch.

## Phase 4: the incremental engine

**Query graph.** Phase 4 includes the persisted demand-driven query graph, continuous clean
validation, and per-definition C++ builds ([`incremental-engine.md`](incremental-engine.md)).
Registered coroutine query kinds use canonical keys and values, record input and nested-query
dependencies in read order, validate red-green on demand, cut off unchanged fingerprints,
propagate durability, cancel cooperatively, and expose recorded inputs and values for clean
comparison. Idle one-in-twenty validation uses a new graph instance, persists audit records,
reports the query and inputs on divergence, and evicts the offending value.
`liquid verify --graph --all` performs a full pass. Direct build execution and every wrapped tool
action are `action_result` graph nodes.

**Frontends.** A versioned C frontend ABI loads replaceable shared or static language modules
outside libliquid, and the native liquify plugin descriptor accepts a versioned presence, lease,
and attribution event callback table ([`frontends.md`](frontends.md)). The bundled tree-sitter fallback fetches 49
exact-commit grammars under archive and per-file SHA-256 checks, registers their extensions, and
discovers units from pinned tag queries where upstream supplies them; syntactic records and
`liquid version --json` carry the grammar-lock digest. The distribution decision and inventory are
in [`grammars.md`](grammars.md). The Clang C++ module emits definition actions and assembles their
outputs into a relocatable translation-unit object. A function-body edit therefore executes only
that definition's new action; recorded header reads invalidate the definitions that use them.

**Symbols.** Liquid #66 adds content-addressed per-translation-unit symbol records from a query
over recorded compile actions, a labelled tree-sitter fallback, graph retrieval, repository access,
and `liquid symbols <path>` ([`symbols.md`](symbols.md)).

**Hot update.** Liquid #32 adds evidence-based hot-update classification for tiers 0–4, signed
patch and restart records, the versioned `liquid-hot` cooperative C runtime for macOS arm64 and
Linux x86_64, sandboxed restart hooks, reconstructible base-plus-patch status, and a drip-driven
daemon target. Tier 2 verifies the #27 descriptors referenced by both signed results and otherwise
returns the tier-3 restart outcome ([`hot-update.md`](hot-update.md)).

**Build streams, tests, and plans.** Public build handles stream queued, started, hit, done,
failed, critical-path, and finished events through the library and daemon. CMake test
registrations become discoverable cached actions, with `liquid build --events` providing JSON Lines
and `liquid test` reporting results ([`../cli/build-events.md`](../cli/build-events.md),
[`../cli/test.md`](../cli/test.md)). Configured repository builds expose complete public action
plans with dependency edges and validated hit state. `repository::build()` and the daemon start the
whole selected build in one call, `repository::root_digest()` exposes the current tree record, and
`liquid build --plan` prints the daemon plan schema as JSON Lines.

**Structural edits.** Liquid #205 adds canonical structural-edit records and repository/daemon
tasks for C and C++ rename, extract, move, and concrete-tree edits. Previews contain minimal text
changes and refusals; apply verifies file digests, writes one drip per file, and records one
attributed operation for whole-edit undo. The bundled tree-sitter frontend exposes concrete syntax,
and `liquid edit` previews or applies through the checkout daemon ([`edit.md`](edit.md)).

**Daemon lifecycle and fan-out** are implemented on macOS and Linux. The CLI starts one locked
process per checkout on first daemon-backed use and attaches over framed canonical CBOR. The
daemon owns capture, maps each drip to the changed graph inputs, processes build, search,
collaborator, and lake hooks in order with durable per-target cursors, reports lag through
`liquid status`, exits when idle, and persists its graph on shutdown. Resident graph values use a
byte ceiling and least-recently-demanded eviction while keys and fingerprints remain. Windows
returns a documented unsupported result until Corosio provides named-pipe transport. The daemon
uses issue #20's public graph interface for persistence, invalidation, and resident-value bounds.
It also performs #23 clean validation during idle slices and reports validation backlog and the
last divergence. Issue #85 persists actor, directory-document, and repository token buckets;
rejects over-budget writes before a CRDT transaction opens; applies the same author limits to
pushes; and reports persistence lag, consumer lag, watermarks, and rejection counts
([`capture.md`](capture.md)).

**Local search and cost planning.** `liquid search`, `repository::search`, and
`repository::plan_search` select BM25F, verified positional trigrams, or a parallel mapped scan.
`liquid search --explain` reports measured inputs, estimates, actual cost, and rejected
alternatives. The content-addressed index lives under `.liquid/index/`. The daemon publishes
changed paths and tombstones after each drip with a durable independent cursor; an atomic manifest
replacement makes the whole change visible at once. Identical blobs are indexed once even when
several paths refer to them. Named refs use snapshot-specific index slots
([`search.md`](search.md)).

**Agent application-data branching** is implemented for PostgreSQL template copies. Persisted
declarations create one isolated branch per session, guarded promotion refuses a changed base,
discard removes disposable data, and status plus `liquid data` expose the live lifecycle. A
session-scoped `wrap` receives branch URLs in a network-enabled sandbox. The reflink capability
selector is implemented; local cluster control metadata remains required for that lifecycle fast
path ([`data-branching.md`](data-branching.md)).

## Phase 5: formats, projection, resolution, and configuration

**Structural merge.** Issue #69 structurally merges concurrently changed source blobs after CRDT
convergence. Disjoint definition edits combine, suspicious proposals remain conflict values, and
the public resolution API and `conflicts`/`resolve` commands preserve explicit review. See
[`structural-merge.md`](structural-merge.md).

**Format registry.** Phase 5 includes the format registry and dependency-free ZIP, tar, HDF5,
Parquet, SQLite, linked-binary, PDF, notebook, office-document, package, wheel, JAR, safetensors,
and non-executing pickle codecs through a versioned C module ABI. Format trees drive opt-in
repository and CLI container diffs, archive entries use `!/` paths in search, and Tier 1 and Tier 2
entries are retained as deduplicated blobs beside the verbatim container. Uncompressed tar has a
deterministic writer, while hard or unsupported format surfaces retain honest tiers and opaque
reasons ([`formats.md`](formats.md)).

**Projection.** Tier A package and image projection and Debian-only Tier B source projection are
implemented. One checked install tree
projects to deterministic `.deb`, `.rpm`, `.apk`, Homebrew bottles, Arch recipes, OCI image
layouts, digest-pinned Helm charts, pinned-runtime AppImages, Flatpak build directories, and
Snaps. A recorded change or producing result projects to an unsigned `.dsc`, deterministic source
archives, and generated Debian policy files. Binary readers derive package relationships;
incompatible ABI and relocation verdicts
refuse by default; and signed `projection/1`, source provenance, and SBOM statements accompany
every artifact
([`projection.md`](projection.md)).

**Dependency resolution** is implemented. Canonical `package/1` and fully bound
`concretised_spec/1` records carry versions, variants, virtual and concrete dependencies, compiler
and target constraints, conflicts, and patches. The public Spack-style parser reports exact error
columns, while directory and repository universes use the documented `packages/` tree. Liquid's
own CDCL engine chooses virtual providers, binds compilers as dependencies, applies target-family
fallback, optimises reuse before freshness and defaults, and returns minimal attributed conflict
cores. Selected graphs are constructed iteratively, so dependency-chain length never consumes
worker-thread stack. `liquid resolve` prints the graph, stable JSON, or package-selection
explanations ([`resolution.md`](resolution.md)).

**Configuration.** `.liquid` is a typed declarative language for package, source, and target
records with repository-local includes, exact source-located diagnostics, canonical formatting and
CBOR lowering, digest-verified source fetch, and ordinary-program generators executed with signed
provenance. `liquid config` checks, lowers, traces, and formats; `liquid build --target` executes
a lowered target through the existing action path and refuses outputs that fail relocation
publication ([`configuration-language.md`](configuration-language.md)).

## Deployment

Liquid #84 adds an operator-control-plane deployment client with digest-pinned interim OCI images,
named environments, status readback, and rollback ([`deploy.md`](deploy.md)).

## Known limits in one place

- The action-result memory cache tier remains separate work.
- Measured transfer timing remains Phase 3 work.
- The containerd proxy snapshotter remains in k0s #14.
- Windows returns a documented unsupported result for the daemon until Corosio provides
  named-pipe transport, and for `wrap`, `verify`, and `replay`.
- Lazy mounts, `wrap`, `verify`, and `replay` need libfuse3 on Linux and macFUSE on macOS.
- The executable-aware delta algorithm id is reserved and unused.
- The data-branch reflink fast path requires local cluster control metadata.
- The open decisions are listed in `CLAUDE.md` under *Decisions*; the licence for liquid and
  liquify is not yet chosen
  ([liquid-bridge #2](https://github.com/oramasearch/liquid-bridge/issues/2)).
