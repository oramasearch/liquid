# liquid

liquid is a content-addressed, coroutine-native version control system and build system in one
program. One chunk store holds source, binaries, packages, images, search indexes and encrypted
data. One dependency graph of pure actions builds, packages, indexes and deploys from that store;
the sandbox observes what each action reads rather than trusting a declaration, and outputs are
content-addressed, so a rebuild that produces identical bytes stops the cascade. Every write to
disk is captured as a signed drip, and nothing is materialised until it is read. On top of the
store sit the incremental query graph, dependency resolution with liquid's own solver, and
projection of one checked install tree to `.deb`, `.rpm`, `.apk`, OCI images and Helm charts.
The premise, the vocabulary, the design axioms and what liquid must do are stated in
[`docs/features/README.md`](docs/features/README.md).

liquid is the tool. [lake](docs/features/18-lake-service.md) is the hosted service where it
collects, and [liquify](docs/features/21-liquify-ide.md) is the IDE. The repository is an Orama
project and one module of
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge), which holds the
action plan.

## Status

The Phase 1 store exit criterion is met: the chunk store, repositories, capture, identity,
encryption, Git import and export, sync, drive and the macOS/Linux checkout daemon are implemented
inside libliquid. The Phase 2 build wrapper, cache, verification and provenance, the Phase 3
remote blobs, replay and delta delivery, the Phase 4 query graph, daemon, search and structural
edits, and the Phase 5 formats, projection, resolution and `.liquid` configuration are implemented
as recorded in [`docs/library/status.md`](docs/library/status.md), which also lists the known
limits. The code is a C++26 library, `liquid::liquid`, plus the `liquid` binary as a thin client
of it.

> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, structured JSON/`.env` encryption, Lake sealed-secret envelopes, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, pluggable KEK custody with trust-domain data-key rotation and revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, structured JSON/`.env` encryption, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, agent write flow control, inline secret scanning, verified redaction, structured JSON/`.env` encryption, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> (`liquid::liquid`, static by default, shared with `BUILD_SHARED_LIBS`) plus the `liquid` binary
> as a thin client of it; liquify and lake link the same library. The public error, digest, identity
> document codec, and signature decoder APIs are implemented, including BLAKE3, SHA-256, and keyed
> BLAKE3 identifiers. Store owners can await a whole-content write-through callback before the
> first local pack write and evict remote-registered local copies without losing their remote
> location. Repository-local BM25F, substring, and regex search over the working tree or
> named refs is implemented with measured cost planning, current mapped scans, stable explanations,
> and atomic per-drip publication. The public waiting operations are lazy
> Boost.Capy tasks, backed by a private Corosio runtime. The local store implements open, put,
> bounded-memory `put_file` and `get_to_file`, get, range read, contains, trust-domain encrypted objects with passphrase, raw-key-file, age/X25519, cloud KMS, lake transit, or Shamir custody,
> versioned data-key rotation and revocation, repository readability reports,
> leased roots, epoch garbage collection, and pack compaction. Multiple repositories can use one
> caller-owned store with repository-scoped checkpoints, journals, and retention leases. Capture subscriptions, default-blocking
> secret scanning, the TAAP attestation profile registry with `scan_result/1`, `reproducibility/1`,
> structured `slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, and repository redaction are implemented. Repository
> metadata also retains verified Lake transparency checkpoints: the public client verifies signed
> witness-quorum checkpoints, all entry and parent hash tiles, exact TAAP inclusion, consistency,
> and identity scans before callers trust a log response.
> structured `slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, `image_provenance/1`, and repository redaction are implemented. Repository
> push and pull, signed metadata exchange, missing-object negotiation, the public sync server with
> change and graph-derived per-reference authorization, mixed and atomic push outcomes, and the
> shared HTTP/1.1 client, `serve sync` adapter, and lake forge review client with its polling event
> stream and installable double are implemented. VFS full materialisation works on every platform;
> HTTP/1.1 client and `serve sync` adapter are implemented. VFS full materialisation works on every platform;
> `liquid serve admission` provides the fail-closed Kubernetes webhook, TLS and control listeners,
> atomically reloaded named policy sets at `/admit/<policy>`, structured CBOR/JSON decisions in
> AdmissionReview status, a bounded local decision stream, local or node-store evidence lookup,
> metrics, REST, OpenAPI, and MCP. Complete
> transparency inclusion verification checks signed checkpoints, witness quorum, tiles, and proofs before log mode accepts a record.
> HTTP/1.1 client and `serve sync` adapter are implemented. Issue #195 adds the persisted
> three-tree drive client, named concurrent conflict values, interrupted-transfer resume,
> per-reference refusal reporting, stable-node path history and restore, `liquid drive`, and the
> daemon drive target. Issue #235 adds persisted online-only, available-offline, and excluded path
> state; pin and unpin commands; xattrs; policy-driven and offline-aware hydration; queued-write and
> hydration-debt status; and GNOME Files and Dolphin badge providers. VFS full materialisation works on every platform;
> lazy mounts use libfuse3 on Linux and optional macFUSE on macOS. Repository creation, working-tree reads
> and writes, recorded moves
> and removals, change cuts, references, snapshots, historical document state, composition,
> conflict values, operation- and session-scoped local-intent undo, advisory
> locks, private speculative agent sessions, semantic file/symbol/range leases, transient presence,
> compaction epochs, scoped delegation attribution, status, public tree/change diff,
> edit-surviving text anchors, paged result, attestation, trace, and SBOM enumeration,
> one-drip streaming file writes and tree imports, and caller-stated write bases with
> stale-reasoning refusal, disjoint re-validation, and forced re-planning records are implemented.
> TAAP/1 identities with rotation and signed drips and changes, the internal
> CRDT metadata document, canonical CBOR codec, and drip journal are implemented behind libliquid.
> Directory CRDT checkpoints prefer compact bytes with checked full-history fallback and reopen as
> sync-capable documents; remote changes are observed through patches, and every automerge apply,
> merge, and sync-receive failure is checked.
> The `liquid` binary is the documented thin client of the Phase 1 repository, capture, VFS, store,
> and identity APIs plus the Phase 2 build wrapper. `liquid gc --explain` reports why retained chunks are live. Repository path
> rules encrypt selected JSON and `.env` leaf values while leaving their structure visible, with
> explicit plaintext reads and key-only historical diff. Whole-file encryption remains the fallback,
> and `find_package(liquid)` works from an install. `liquid import git` preserves reachable local Git
> history and its identity mapping; `liquid export git` writes a standalone Git repository and reports
> compatibility losses. Strict DER, CMS, and X.509 parsing plus verification-only RSA and ECDSA
> validate RFC 3161 checkpoint timestamps and RFC 4998 renewal chains against explicitly pinned
> TSA trust anchors at each token's historical generation time.
> The CLI now validates every option from one typed command table and generates its Markdown,
> stable JSON, and installed man-page reference from that table. Executable guides cover an
> existing wrapped build, a first repository and sync peer, and the current keys-and-encryption
> boundary. CI renders the complete documentation tree as a downloadable static-site artifact.
> The `liquid-offline` suite reruns every CLI test in liquid's macOS or Linux sandbox with
> networking denied and verifies the explicit push, pull, remote-execution, and remote-cache
> refusals before their handlers start.
## Build and run

liquid needs a modern Clang (Homebrew LLVM on macOS, `clang++-17` or newer on Linux), CMake 3.28
or newer, Ninja, OpenSSL and zlib. Linux also needs libfuse3. On macOS, macFUSE is optional and
enables lazy mounts and `liquid wrap`.

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The static library is `build/libliquid.a` and the command-line binary is `build/liquid`. A first
repository:

```sh
mkdir example && printf 'first\n' > example/readme.txt
(cd example && ../build/liquid init . && ../build/liquid status --json && ../build/liquid cut "import")
```

`cmake --install build --prefix <prefix>` installs `bin/liquid`, `lib/libliquid.a`,
`include/liquid/` and the CMake package. Prerequisites, options, the install layout and the
documentation checks are in [`docs/build/local.md`](docs/build/local.md). Consuming the library
from another CMake project is in [`docs/library/libliquid.md`](docs/library/libliquid.md).

## Documentation

[`docs/README.md`](docs/README.md) indexes every page. The most useful ones:

- [`docs/features/README.md`](docs/features/README.md): the premise, names, design axioms, what liquid must do, and the 25 feature areas.
- [`docs/library/status.md`](docs/library/status.md): what is implemented today, by phase, and the known limits.
- [`docs/library/api-map.md`](docs/library/api-map.md): the public headers of libliquid and the rules they obey.
- [`docs/cli/README.md`](docs/cli/README.md) and [`docs/reference/cli/`](docs/reference/cli/): the command-line client and its generated reference.
- [`docs/guides/`](docs/guides/): executable workflows for a first repository, a wrapped build, and keys and encryption.
- [`docs/research/99-synthesis/`](docs/research/99-synthesis/): the argument in one place, drawn from the 27 research topics.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) holds the repository rules: the vocabulary, the document structure, the
checks to run before finishing, the recorded decisions, and the org conventions inherited from
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge).
[`AGENTS.md`](AGENTS.md) restates the non-negotiables for every agent.

## Licence

liquid and liquify will be open source; the licence text is not yet chosen
([liquid-bridge #2](https://github.com/oramasearch/liquid-bridge/issues/2)). Until then
`oramasearch/liquid` is private. Third-party notices are in [`NOTICE`](NOTICE).
