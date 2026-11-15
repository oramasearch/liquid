# Implementation status

**VFS conformance (#301).** Nine common cases pass against the direct core calibration backend,
covering file operations, recorded inputs, attributed writes, partial hydration, synthetic times,
xattrs and read-only refusal. A deliberate wrong-size mutation fails with the operation and
expectation. The unchanged libfuse entry skips on this Mac because macFUSE is absent. Agreement
with liquid's own core is weaker than agreement with a known-good filesystem; the Linux libfuse
baseline remains owed against #299 ([`vfs.md`](vfs.md#backend-conformance-301)).

Issue #2: ordinary and nested splits own their referenced chunks and expose original signed
history after source removal. Shared-store sources produce standalone destinations. Publication
and limitations are documented in [`split.md`](split.md).

This page states what libliquid and the `liquid` binary implement today, by phase, and what each
phase still lacks. It is the authoritative implementation status record; the root `README.md`
and `CLAUDE.md` keep short summaries and link here. The
[historical reconciliation](readme-history.md) records how duplicate inventories were combined for issue #284. Each paragraph names the page that holds
the design and verification detail. When a change makes a sentence here false, correct it in the
same commit.

**Code shape.** Code is a C++26 **libliquid** (`liquid::liquid`, static by default, shared with
`BUILD_SHARED_LIBS`) plus the `liquid` binary as a thin client of it. Other programs consume the
same library through its exported CMake target. The public waiting operations are lazy Boost.Capy
tasks, backed by a private Corosio runtime ([`runtime.md`](runtime.md)). Issue #285 completes
the installed static dependency closure: `find_package(liquid CONFIG REQUIRED)` and `liquid::liquid` support repository
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

**Identity export (#294).** `liquid identity show --document` emits only the signed public TAAP/1
document as unpadded base64url canonical CBOR, with no newline. Software, FIDO2, and PIV private
keys remain local. Combining `--document` with `--json` or rotation returns a usage error (exit 2).
The public `signer::document()` accessor supplies the stored signed document to the existing codec
([`identity.md`](identity.md)).

**Signer attribution (#383).** Verification fails closed: a signer's name is bound to a key only by
that identity's document chain, which must start at the initial document named after its own key.
`verify(actor, ...)` reads the chain from the local identity store and returns an unknown-signer
error when there is none; `verify(actor, chain, ...)` takes a carried chain. Sync carries every
author's chain as an `identity` frame and stores it before checking signatures; `liquid identity show
--chain` and `liquid identity trust` move a chain by hand. Release approval, hot update, build
policy, admission, worker results, and the build cache all refuse an unknown signer
([`identity.md`](identity.md#how-a-verifier-obtains-and-trusts-another-persons-chain)). Every
decision that trusts an identity requires that chain (Steve, 2026-11-14; #429). A signature with no
chain is reported by `check_signature` as signed by an unverified key, `repository::drips()` and
`repository::log()` mark such records `identity_verified == false`, and `liquid log` shows them as
`signed by an unverified key`, never as the name they claim
([`identity.md`](identity.md#every-decision-that-trusts-an-identity-requires-a-verified-chain)).
Path history, operation records, line attribution and change attribution carry the same flag, so
`liquid blame`, `history`, `ops`, `why` and `wrap` show an unverified author the same way, and
their JSON carries `"identity_verified"` (#508). The sync push rate limit is charged to the
authenticated pusher, never to a record's claimed author (#508).
**FIDO2 assertion bytes (#81).** FIDO2 EdDSA and ES256 signatures are verified over
`authenticatorData || clientDataHash`, the bytes CTAP2 authenticators sign; the earlier
double-hashed form is rejected. Proof is independent OpenSSL vectors and protocol-conforming doubles;
no physical authenticator run exists yet ([`identity.md`](identity.md)).

**Compromised keys (#391).** A key named in a compromise declaration verifies no signature through
`verify`: its `signed_at` is the signer's own claim and can be backdated. `verify_with_trusted_time`
accepts one only with an RFC 3161 token or RFC 4998 evidence record over the signature that a
caller-trusted TSA dates before the declaration. No record format carries such evidence yet, so
every signature by a declared-compromised key is refused today
([`identity.md`](identity.md#rotation)).

**Identity key custody (#402).** Every identity file is created owner-only and atomically (mode
0600 through `open(O_CREAT|O_EXCL|O_NOFOLLOW)`, or an owner-only DACL on Windows, then a rename).
A new software identity seals its wrapping key under a passphrase (`LIQUID_IDENTITY_PASSPHRASE_FILE`
or a terminal prompt), the macOS Keychain, or a key custody URI (default: `keychain` on macOS,
`passphrase` elsewhere); a clear `.wrapping.key`, which leaves the key protected only by file
permissions, is written only for explicit `file` custody. Older identities keep loading;
`liquid identity show` reports and warns about their custody, and `liquid identity custody --to`
migrates them ([`identity.md`](identity.md#how-the-software-signing-key-is-protected-at-rest-liquid-402)).

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
runs the same invariants against disk and memory. A collection publishes its object map before it
commits tombstones, so a crash between them never leaves an object naming a tombstoned chunk
(liquid #337). Store owners can await a whole-content write-through callback
before the first local pack write and evict remote-registered local copies without losing their
remote location. Multiple repositories can use one caller-owned store with repository-scoped
checkpoints, journals, and retention leases. `liquid gc --explain` reports why retained chunks are
live. Digest-addressed near-duplicate chunk deltas and resemblance-selected bases reduce
stored bytes; repacking rebases delta chains and garbage collection retains their transitive
bases ([`store.md`](store.md), [`gc.md`](gc.md)). A long-running owner opens with
`store_options::exclusive_owner`: one handle owns a disk root across processes, and a successor
waits up to `owner_wait` for a draining or killed predecessor to release it (liquid #321,
[`store.md`](store.md#process-ownership-and-hand-off)). Without an owner, several processes (the
checkout daemon, the CLI, an IDE) write one disk root safely: each mutation takes the root's write
lock and reloads what another process published first, temporaries are unique per call, opening
writes nothing, and an index whose manifest names a missing run is rebuilt from the packs and
recorded in `recoveries.log`, which `liquid status` prints (liquid #526,
[`store.md`](store.md#several-writers-without-an-owner)). Repository documents are still replaced
whole by each process, so two processes saving one repository at once can drop each other's drips
from the published document. A repository handle's own leases name their holder process, are
released when the handle ends, and are collected by the next open when their process has ended,
so a checkout keeps only its live handles' leases; `liquid status` prints the count (liquid #527,
[`gc.md`](gc.md)). A shared-store repository's own state
(`LIQUID_REPO`, `LIQUID_JOURNAL`, session state, and replication cursors) is backend keys under
`repositories/<id-hash>/`, so a backend with no local root holds the whole repository; create is
exclusive through `store_backend::put_if_absent` (liquid #326,
[`store.md`](store.md#repository-state-through-the-backend)). A checkpoint's journal removal and
a session's state removal on publish or discard end with a barrier, so what the operation
acknowledged is durable when it returns (liquid #343). `store_backend` states its ordering
contract between barriers: a crash may keep any subset of one interval's writes, each key's own
writes in order, and the store depends on nothing stronger; the in-memory backend's subset crash
mode checks the store, repository and session crash suites against it (liquid #344,
[`store.md`](store.md#ordering-between-barriers)). A store on a backend without a local
root takes the same ownership through an injected `store_options::owner` whose grant carries a
fence: the backend's fenced view sends it with every mutation, `put_if_absent` included, so the
remote refuses a stale holder, and a lost grant refuses every later mutation with `errc::locked`
(liquid #327,
[`store.md`](store.md#injected-owners-and-fencing)). `repository::transfer` copies one repository,
whole, between two open shared stores: every reachable object with its digest, then its lease
records, then its state keys with `LIQUID_REPO` last, through the destination's fenced backend; a
rerun after an interruption copies only what is missing and a verify mode reads both stores and
reports every difference (liquid #328, [`store.md`](store.md#whole-repository-transfer)). Encrypted
objects are not transferred yet.
No store write rewrites bytes already written: index compaction sizes its run in a first merge and
writes the header, bloom and records once, in order, in a second, so every backend object is
appended in order or replaced whole, which Zetta's object storage requires (liquid #332,
[`store.md`](store.md#index)).

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

**Exclusive checkout locks.** Configured path patterns require holder-aware writes and release.
Lock records carry holder, acquisition time, and reason; signed offline reconciliation produces
conflict values, connected authority uses a callback, and `liquid lock`/`unlock` forwards lock
snapshots to daemon targets without advancing their drip cursors ([`locks.md`](locks.md)).

**Capture, scanning, and attestations.** Capture subscriptions, default-blocking secret scanning,
the TAAP attestation profile registry with `scan_result/1`, `reproducibility/1`, structured
`slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, `image_provenance/1`, `release_policy/1`,
and `release_approval/1`, and repository redaction are implemented. JWT detection uses linear work
and constant auxiliary storage even with repeated invalid prefixes (liquid #440)
([`capture.md`](capture.md), [`secret-scanning.md`](secret-scanning.md),
[`attestations.md`](attestations.md), [`release-approval.md`](release-approval.md)).

**Encryption of structured values.** Repository path rules encrypt selected JSON and `.env` leaf
values while leaving their structure visible, with explicit plaintext reads and key-only
historical diff. Whole-file encryption remains the fallback
([`leaf-encryption.md`](leaf-encryption.md), [`sealed-secrets.md`](sealed-secrets.md)).

**Git.** `liquid import git` preserves reachable local Git history and its identity mapping;
`liquid export git` writes a standalone Git repository and reports compatibility losses
([`git-interop.md`](git-interop.md)). Object inflation and delta reconstruction are bounded
per read, including cross-pack reference bases; nonprogressing offset bases and excessive
delta depth are rejected (#443).
Limits are 4 GiB per object/program/result, 1,024 delta links, and twice the final payload size
plus 64 MiB of decoded/reconstructed bytes per read.

**Sync and review.** Repository push and pull, signed metadata exchange, missing-object
negotiation, the public sync server with change and graph-derived per-reference authorization,
mixed and atomic push outcomes, bounded client retries of a `503` or `429` that carries
`Retry-After`, and the shared HTTP/1.1 client, `serve sync` adapter, and lake
forge review client with its polling event stream and installable double are implemented
([`sync.md`](sync.md), [`forge-client.md`](forge-client.md)). HTTPS verifies against
`SSL_CERT_FILE` or OpenSSL's default trust store, on macOS too (liquid #368).
Chunk uploads are bounded by a 64 MiB
decoded-size budget per request that counts repeated frames, and are stored one decoded frame at a
time (liquid#435).
Send rejects any missing-object
response containing a digest outside the current offered root closure before any upload
(liquid #446).

**Drive.** Issue #195 adds the persisted three-tree drive client, named concurrent conflict values,
interrupted-transfer resume, per-reference refusal reporting, stable-node path history and restore,
`liquid drive`, and the daemon drive target. Issue #258 binds imported active directory documents
to each destination repository's actor, so two clients retain both named histories after concurrent
offline edits. Issue #235 adds persisted online-only,
available-offline, and excluded path state; pin and unpin commands; xattrs; policy-driven and
offline-aware hydration; queued-write and hydration-debt status; and GNOME Files and Dolphin badge
providers. Issue #247 adds `liquid clone` over the same client, including first-pull identity
adoption, persisted remote configuration, full plain-checkout materialization, and stable JSON
output ([`drive.md`](drive.md), [`../cli/README.md`](../cli/README.md)). Issue #423 makes filesystem
ingestion skip symlinks and pin regular-file sources through no-follow descriptor walks on macOS
and Linux; unsupported platforms refuse ingestion.

**VFS.** Full materialisation writes a plain directory; lazy mounts use libfuse3 on Linux and
optional macFUSE on macOS ([`vfs.md`](vfs.md)). Tree entry names are validated at decode, encode,
repository writes, Git import and both materialisers, and materialisation creates entries
exclusively without following symbolic links (liquid #386,
[`object-model.md`](object-model.md#records-in-the-store)).
**VFS.** Read-only mounts and immutable `deps/` paths refuse setting and removing Drive xattrs
with `EROFS`, retain persisted settings, and report denied writes with the caller PID (#445).
Full materialisation writes a plain directory; lazy mounts use libfuse3 on Linux and
optional macFUSE on macOS ([`vfs.md`](vfs.md)).

**Transparency and trust.** Repository metadata retains verified Lake transparency checkpoints:
the public client verifies signed witness-quorum checkpoints, all entry and parent hash tiles,
exact TAAP inclusion, consistency, and identity scans before callers trust a log response. Each
full or partial tile is reauthenticated through its parents to the selected checkpoint; cached
tile bytes receive the same complete walk, and consistency verification completes that walk before
it requests a proof. HTTP log source construction reports invalid URLs through the public
result model; legacy request operations retain the validation error. `log scan`, `log consistency`,
and `log verify` reject malformed URLs locally with exit 2 (liquid #289).
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
failure reason. Positive checkpoint age bounds use the signed `issued_at`, refuse missing time,
expiry, and future skew beyond 60 seconds, and apply on every decision; zero is invalid at policy
load and an omitted bound disables freshness enforcement. The log attests its own clock.
Metrics, REST, OpenAPI, and MCP expose the same record
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
downloadable static-site artifact ([`../build/ci.md`](../build/ci.md)). The `liquid-offline` suite
reruns CLI tests in the macOS or Linux sandbox with networking denied and verifies intentional
network refusals before handlers start ([`../cli/README.md`](../cli/README.md)).

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
The exec-time hook revalidates conditional inputs by BLAKE3-256 and reads only a plan at liquid's
own location whose bytes hash to the digest liquid passed (#401). Process keys are BLAKE3-256 over
executable, working directory and argv, and a saved wrap state from before that change is ignored
and rebuilt (#449).

**Verification.** `liquid verify` re-executes cached actions in fresh sandboxes, compares exit
status and every output digest, signs a `reproducibility/1` attestation, and permanently
quarantines a divergent result. `verify --log <log-url>` additionally requires inclusion for every
agreeing reproducibility attestation; missing, tampered, and unavailable inclusion fail the command,
while an omitted log keeps local-only verification. `wrap` schedules one-in-20 graph and action
samples on the shared runtime by default.

**Outputs and inputs.** New signed results carry canonical output-tree digests.
`wrap --input <name>=<result|tree>` and direct named inputs mount upstream bytes read-only below
`deps/<name>/`; an identical normalised upstream tree stops the downstream cascade. Directory
modes, `ar` headers, and Python bytecode are normalised before storage, with the rule set bound
into the sandbox digest. Output collection opens each path with `O_NOFOLLOW` and fails an action
whose declared outputs contain a symbolic link or special file before reading it; normalised bytes
go to a parent-owned staged copy, never back into the output (liquid #425). `wrap --install-prefix` stages a canonical `bin/lib/include/share` install
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
and `liquid serve reapi` exposes liquid's CAS and ActionCache ([`reapi.md`](reapi.md)); an
ActionCache action digest must be lowercase hexadecimal of its algorithm's length before it names a
file, and entries are never opened through a symbolic link (liquid #428). The HTTP/2
transport under both enforces the limits it advertises (concurrent streams, header list and table
sizes, frame size, flow control) and closes on rapid reset, CONTINUATION, PING and SETTINGS floods
([`http2.md`](http2.md), liquid#412). The server reads each request one bounded message at a time
and refuses a message over 5 MiB with `RESOURCE_EXHAUSTED` (#451). Both REAPI listeners bound
retained ByteStream uploads by size, total allocated capacity, count and idle expiry and active
Write deadlines; failed Writes release their buffers (liquid #444). Liquid's
action-digest scheduler and TAAP-signed worker fleet are implemented, including one bounded requeue
after worker loss, size-class placement from #18's recorded decision, REAPI Execution and
ActionCache, and deployable health, metrics, REST, OpenAPI, and MCP surfaces
([`workers.md`](workers.md)). The scheduler and workers authenticate each other with pinned TAAP
identity documents (`--trust-worker`, `--trust-scheduler`): every worker request and every lease
is signed and replay-checked, lease tokens are 128 random bits, and a result is accepted only under
the leased worker's pinned key (liquid #416). The standard REAPI services and `Submit` remain
unauthenticated.

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
`liquid version --json` carry the grammar-lock digest. A grammar can carry a locked, exact-text
patch to a fetched file; the Swift scanner carries upstream's fix for a heap read past a
zero-byte allocation (#462). The distribution decision and inventory are
in [`grammars.md`](grammars.md). The Clang C++ module emits definition actions and assembles their
outputs into a relocatable translation-unit object. A function-body edit therefore executes only
that definition's new action; recorded header reads invalidate the definitions that use them.

**Structural diff and support tiers.** The graph-owned canonical parse is shared by build units,
symbols, and source diff. Structural matching reports moves, semantic edits, formatting, and
comments; `liquid diff --structural` labels its structural, text, or binary fallback. The public
support matrix derives language and format rows from the loaded registries and declares platform
wrapper backends; `liquid tiers` emits text, JSON, and generated feature tables
([`structural-diff.md`](structural-diff.md), [`support-tiers.md`](support-tiers.md)).

**Symbols.** Liquid #66 adds content-addressed per-translation-unit symbol records from a query
over recorded compile actions, a labelled tree-sitter fallback, graph retrieval, repository access,
and `liquid symbols <path>` ([`symbols.md`](symbols.md)).

**Hot update.** Liquid #32 adds evidence-based hot-update classification for tiers 0–4, signed
patch and restart records, the versioned `liquid-hot` cooperative C runtime for macOS arm64 and
Linux x86_64, sandboxed restart hooks, reconstructible base-plus-patch status, and a drip-driven
daemon target. Tier 2 verifies the #27 descriptors referenced by both signed results and otherwise
returns the tier-3 restart outcome ([`hot-update.md`](hot-update.md)). Issue #305 gives the tier-1
demo a scoped, bounded socket path and makes overlong runtime endpoints report the native limit
and path ([socket fixtures](../build/socket-fixtures.md)).

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
process per checkout on first daemon-backed use and attaches over framed canonical CBOR, on a
socket in a private per-user directory that refuses other users by peer credentials (#397). The
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
reasons ([`formats.md`](formats.md)). The Parquet and PDF readers charge every allocation sized by the file, and
the bytes they decompress, to one byte budget per unpack before making it (liquid #433).
One unpack's depth, byte and entry budgets bound every codec
it reaches, including the package and document codecs that decode their own members through the
registry (liquid#438).
HDF5 rejects filter pipelines longer than the 32-bit
chunk mask before decoding any dataset layout (liquid #486).
The RPM reader charges the header strings it copies and its
The RPM reader charges the header strings it copies and its
decoded payload to one byte budget per read (liquid #473).
RPM reads reject truncated cpio name and file padding before copying members or accepting the
trailer, and check signature-header padding without wrapping (liquid #439).
reasons ([`formats.md`](formats.md)).
Mach-O inspection caps each symbol name at 64 KiB and charges retained names and records to the
registry caller's byte budget, or 16 MiB for other callers; exhaustion preserves partial facts
and all consumers report truncation. ABI comparisons and relocation approval fail closed for
incomplete inspection (liquid #475).

PE names have a 64 KiB length cap, offset
deduplication, and one allowance for retained names and records; capped names or exhausted budgets
produce partial, truncated results (liquid #474).

ELF name copies and their records share one cumulative inspection allowance, including section,
symbol, version, dynamic-path, and DWARF names and generated signatures. A 64 KiB name cap or an
exhausted allowance stops collection before copying and reports truncation (liquid #479).
Safetensors charges tensor bytes cumulatively before copying
and truncates at the byte or entry limit, including for overlapping ranges (liquid #476).

**Projection.** The CLI resolves ordinary-repository changes and trees without build configuration;
`liquid tree` prints the recorded root digest. Result-ID projection retains build-repository
lookup ([executable guide](../guides/project-from-files.md)). Tier A package and image projection and Debian-only Tier B source projection are
implemented. One checked install tree
projects to deterministic `.deb`, `.rpm`, `.apk`, Homebrew bottles, Arch recipes, OCI image
layouts, digest-pinned Helm charts, AppImages with vendored and hash-pinned runtimes, Flatpak build
directories, and Snaps. A recorded change or producing result projects to an unsigned `.dsc`,
deterministic source
archives, and generated Debian policy files. Binary readers derive package relationships;
incompatible ABI and relocation verdicts
refuse by default; and signed `projection/1`, source provenance, and SBOM statements accompany
every artifact
([`projection.md`](projection.md)).

**Platform projection and signing.** Projection also produces reproducible unsigned macOS
packages, explicitly non-reproducible disk images, deterministic MSIX, and winget manifests.
Native signing records bind unsigned and shipped digests, identity, and notarisation or timestamp
evidence through `platform_signature/1`. The projection page does not yet record a credentialed
Apple notarisation proof ([`projection.md`](projection.md#unsigned-projection-and-platform-signing)).

**Install intent and image-based OS projection.** Canonical `install_intent/1` records generate
Debian, RPM, and Alpine lifecycle scripts. Their digests bind to projection and SBOM records;
ownership checks detect conflicting paths, explicit replacements derive package relationships,
and install/list/rollback use atomic projected-tree generations with interruption recovery.
Staging never follows a symbolic link out of the generation, and intent links are created after
every intent's directories, users and groups (liquid #385).
Digest-pinned base and overlay trees also produce OSTree commits and bootc OCI layouts, with
separate commit and image digests. Projection is distinct from the Phase 7 boot workflow
([`projection.md`](projection.md)).

**Dependency resolution** is implemented. Issue #287 connects `.liquid` declarations to directory
and repository universes, provides `config lower --output-record` for a selected canonical package,
and diagnoses an empty universe before solving. The [executable guide](../guides/resolve-packages.md)
checks source-only resolution and record equivalence. Canonical `package/1` and fully bound
`concretised_spec/1` records carry versions, variants, virtual and concrete dependencies, compiler
and target constraints, conflicts, and patches. The public Spack-style parser reports exact error
columns, while directory and repository universes use the documented `packages/` tree. Liquid's
own CDCL engine chooses virtual providers, binds compilers as dependencies, applies target-family
fallback, optimises reuse before freshness and defaults, and returns minimal attributed conflict
cores. Selected graphs are constructed iteratively, so dependency-chain length never consumes
worker-thread stack. `liquid resolve` prints the graph, stable JSON, or package-selection
explanations ([`resolution.md`](resolution.md)).

**Configuration.** `.liquid` is a typed declarative language for package, source, target,
install-intent, and exclusive-checkout rule records with repository-local includes, exact
source-located diagnostics, canonical formatting and CBOR lowering, digest-verified source fetch, and ordinary-program generators executed with signed
provenance. `liquid config` checks, lowers, traces, and formats; `liquid build --target` executes
a lowered target through the existing action path and refuses outputs that fail relocation
publication ([`configuration-language.md`](configuration-language.md)).

**Spack import.** A pinned Spack export maps package metadata into formatter-canonical `.liquid`
documents and `package/1` records, pins patches, and reports translated fields and losses in
`import-report/1`. The committed 500-package fixture checks resolution and an introduced HDF5
conflict; an opt-in Linux subset exercises configured builds and relocation
([`import-spack.md`](import-spack.md)).

## Deployment

Liquid #84 adds an operator-control-plane deployment client with digest-pinned interim OCI images,
named environments, status readback, and rollback ([`deploy.md`](deploy.md)).

## Build hardening, sanitizers and fuzzing

Liquid #407 hardens every build by default (stack protector, stack clash and control-flow
protection where supported, standard library assertions, PIE, full RELRO on Linux, and
`_FORTIFY_SOURCE=3` in optimised builds), adds `-DLIQUID_SANITIZE=<list>` covering the C sources
and every dependency, and adds eleven libFuzzer harnesses under `fuzz/` for the sync framing,
object records, drips, CBOR, identity documents, HTTP/1, HTTP/2, the manifest and lock records,
Git objects and protobuf. Their first runs found five defects, fixed in the same change
(#457 to #461). How to run them and the recorded runs: [`../build/hardening.md`](../build/hardening.md).

## Known limits in one place

- The action-result memory cache tier remains separate work.
- Measured transfer timing remains Phase 3 work.
- The containerd proxy snapshotter remains in k0s #14.
- Windows returns a documented unsupported result for the daemon until Corosio provides
  named-pipe transport, and for `wrap`, `verify`, and `replay`.
- Lazy mounts, `wrap`, `verify`, and `replay` need libfuse3 on Linux and macFUSE on macOS.
- A signer is attributed only through an identity chain the verifier holds (#383). Peers built
  before #383 send no identity frames, so their records arrive as unknown signers until their
  chain is trusted by hand. History signed before #448 by an identity whose chain was never kept
  is listed as signed by an unverified key and satisfies no policy; it is not migrated (#452). An
  identity that recovered from a compromise without its old key's
  signature is trusted elsewhere only by `liquid identity trust --accept-compromise-rotation`.
  Who may issue a release policy or a hot-update patch is still open (#384, #388).
- Signatures by a key later declared compromised are refused even when made before the
  declaration (#391): no signing path requests an RFC 3161 token and no drip, change, approval or
  attestation record carries one, and the CLI configures no TSA roots.
- A host without a terminal, a passphrase file or another custody cannot create a software
  identity until `LIQUID_IDENTITY_KEY_CUSTODY` is set (#402); `file` is the explicit opt-in for
  file-permission-only protection. Linux has no Secret Service custody (no D-Bus client), and the
  macOS Keychain custody has no automated test.
- The executable-aware delta algorithm id is reserved and unused.
- The data-branch reflink fast path requires local cluster control metadata.
- The projection page has no recorded credentialed Apple notarisation proof; image booting is
  outside the projection implementation.
- Tier B source projection is Debian-only; other distribution source-policy formats are deferred.
- liquid is released under the MIT licence.
- Fuzz harnesses have short recorded runs only; the 24 CPU-hour runs are
  [#464](https://github.com/oramasearch/liquid/issues/464), harnesses for the remaining parsers
  [#463](https://github.com/oramasearch/liquid/issues/463), and the tree-sitter-swift scanner's
  out-of-bounds read under AddressSanitizer [#462](https://github.com/oramasearch/liquid/issues/462).
