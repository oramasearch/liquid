# liquid — features

This tree states what liquid **does**. It is the counterpart to
[`docs/research/`](../research/), which states what the world currently does and
why it is insufficient.

Every feature document here should be traceable to evidence in the research tree.
Where a feature exists because an incumbent tool fails at something, the document
says so and links to the analysis.

## The premise

liquid is a version control system, build system, package manager, artifact
service and runtime in one program. The name is the thesis: **software changes
form**. Source becomes objects, objects become libraries, libraries become
packages, packages become images, images become running processes — and liquid
moves a project along that chain in either direction, computing only the minimum
delta at each step, distributing the work across many machines, with a
cryptographic chain of custody throughout.

It replaces, or subsumes the role of: git, GitHub, Hugging Face, Spack, Nix,
`docker build`, CMake, Autotools, ccache/sccache, remote build execution (Bazel's
and REAPI's), an artifact registry, a code search engine, a PaaS, Dropbox, Clerk
and Vault.

**[lake.live](18-lake-service.md)** is the hosted expression of all of it: liquid is
the tool, lake is where it collects. A mix of **GitHub, Google Drive and Railway**
over one content-addressed store, with identity, secrets and PKI.

## Names and units

| Name | What it is |
|---|---|
| **liquid** | The tool. The CLI and the daemon. |
| **lake** | Where liquid collects — the hosted service at `lake.live`. |
| **liquify** | The IDE. |
| **drip** | One captured write to disk. The atom of history — automatic, continuous, never explicit, and signed by the party that made it, human or agent. |
| **change** | A named, reviewable aggregation of drips. What another system would call a commit or a pull request. |

Drips are the record; changes are the narrative. Nothing else versions at this
granularity, and it is what makes "compile the minimum delta as you author" literal
rather than aspirational.

## Design axioms

These hold across every feature. If a proposed feature violates one, the feature
is wrong.

1. **One content-addressed chunk store.** Source, binaries, packages, images,
   search indexes and encrypted data are all chunks in one store. There is no
   "large file mode" because there is no distinction to make.
2. **One dependency graph of pure actions.** An action is a pure function from an
   input Merkle tree to outputs, addressed by digest. Builds, packaging, indexing
   and deployment are all actions.
3. **Dependencies are observed, never declared.** The sandbox records what an
   action actually read. Under-declaration becomes impossible; over-declaration
   becomes unnecessary.
4. **Outputs are content-addressed.** A rebuild producing identical bytes stops the
   cascade. This is what kills "build the world".
5. **Nothing is materialised until it is read.** Checkouts, build outputs,
   artifacts and container filesystems are all lazy.
6. **Reproducibility is enforced, not aspirational.** It is load-bearing for
   caching, substitution and provenance.
7. **Provenance is a byproduct.** Every action already records everything an
   attestation needs. Signing is automatic.
8. **Everything works offline, degraded but correct.** Remote cache, remote
   execution and the artifact service are accelerators, never requirements. The
   `liquid-offline` CTest suite runs every `liquid-cli` case in liquid's platform
   sandbox with networking denied and pins every intentional network refusal.
9. **Every write to disk is a versioned change.** Capture is automatic and
   continuous. There is no step at which history begins.
10. **Every change is attributable.** Every drip carries a TAAP/1 signature from an
    identified party — human or agent — and the chain is append-only and tamper-evident
    ([25](25-identity-attestation-and-transparency.md)).
    [TAAP/1](../standards/taap-1-spec-draft.md) is the security model: every claim
    liquid makes about a user, an agent, a document, a build or an artifact is a signed
    attestation, and history is an append-only, tamper-evident log with a witness
    quorum, optionally anchored to a public chain. On by default.

## What it must do

The commitments the feature areas below exist to meet. Each links the area that
specifies it.

- Version anything, including multi-terabyte binary assets, from day one
  ([01](01-object-model-and-storage.md)).
- **Compose repositories**: nest, split, and merge two unrelated codebases with
  history intact. Git cannot do this; it is liquid's headline feature
  ([02](02-version-control.md)).
- Diff *inside* zip, HDF5, Parquet, SQLite and OOXML, not just their bytes
  ([03](03-diff-and-merge.md)).
- Build incrementally as you type, with a build cache and compiler cache built in
  ([04](04-build-engine.md), [05](05-caching.md)).
- Resolve dependencies at Spack's level of expressiveness
  ([07](07-dependency-resolution.md)).
- Produce Tier A packages and images plus Debian Tier B source packages from one build
  ([08](08-packaging-and-images.md)).
- Serve builds, with a transparency log and signed provenance
  ([09](09-artifact-service.md), [10](10-supply-chain.md)).
- Store encrypted secrets without destroying deduplication
  ([11](11-secrets-and-encryption.md)).
- Search full text and code, including inside published artifacts ([12](12-search.md)).
- Deploy to a minimal Kubernetes distribution, a private, upstream-synced copy of
  [k0s](https://github.com/oramasearch/k0s) modified for drips, with a Railway-style
  experience ([13](13-deployment-runtime.md)).
- Present a Dropbox/Drive-style interface for non-developers, local and web
  ([15](15-consumer-sync-and-web.md)).
- Review changes, not branches, so review survives a rebase and stacking is native
  ([02](02-version-control.md), [18](18-lake-service.md)).
- Host models and datasets with training provenance: the chunk store Hugging Face
  already migrated to, plus the attestation chain a model hub cannot offer
  ([09](09-artifact-service.md)).
- Give you an editor with no language server, because the editor and the build are
  the same incremental graph ([21](21-liquify-ide.md)).
- Attribute everything, users, agents, documents, builds and artifacts, with signed
  [TAAP/1](../standards/taap-1-spec-draft.md) attestations, in an append-only log with
  independent witnesses, optionally anchored to a public chain for timestamping that
  does not require trusting us ([25](25-identity-attestation-and-transparency.md)).
- Provide identity, a runtime secrets service, and PKI: one CA serving signing, mTLS,
  TLS and SSH, because keyless signing already *is* certificate issuance
  ([19](19-identity.md), [20](20-secrets-service-and-pki.md)).
- Support every language, platform and operating system, with the tier of support
  stated honestly rather than implied ([16](16-universality.md)).

## Feature areas

| # | Area | Summary |
|---|---|---|
| [01](01-object-model-and-storage.md) | Object model and storage | The chunk store, Merkle DAG, large-file support, and how everything else sits on it. |
| [02](02-version-control.md) | Version control | History, branching, first-class conflicts, and **repository composition** — nesting, splitting and merging unrelated codebases. |
| [03](03-diff-and-merge.md) | Diff and merge | Structural diff for source, format-aware diff **inside** zip, HDF5, Parquet, SQLite and OOXML, with a generated codec-tier table. |
| [04](04-build-engine.md) | Build engine | The incremental query graph, actions, typed `.liquid` targets, early cutoff, compile-as-you-type, and local critical-path traces. |
| [05](05-caching.md) | Caching | Built-in build/compiler caching with content-addressed outputs, miss explanations, byte statistics, and phase attribution. |
| [06](06-distributed-execution.md) | Distributed execution | Remote build farm, REAPI compatibility, build without the bytes. |
| [07](07-dependency-resolution.md) | Dependency resolution | Typed package rules and Spack-grade solving over variants, compilers, targets, virtual providers, content-addressed outputs, and ABI compatibility are implemented. |
| [08](08-packaging-and-images.md) | Packaging and images | Tier A package and image projection plus Debian Tier B source packages, generated install scripts, ownership conflict checks, atomic generations, and ABI gates. |
| [08](08-packaging-and-images.md) | Packaging and images | Projection to native packages, Homebrew, Arch, OCI, Helm, AppImage, Flatpak, and Snap, with ABI and relocation gates. |
| [07](07-dependency-resolution.md) | Dependency resolution | Typed package rules and Spack-grade solving over variants, compilers, targets, virtual providers, content-addressed outputs, and ABI compatibility are implemented; a pinned Spack export seeds the package universe with explicit losses. |
| [08](08-packaging-and-images.md) | Packaging and images | Projection to `.deb`, `.rpm`, `.apk`, OCI images and more, with implemented ABI facts ready for the package gate. |
| [09](09-artifact-service.md) | Artifact service | Serving builds, chunk-level distribution, transparency log, retention. |
| [10](10-supply-chain.md) | Supply chain and signing | Signed builds, hardware-held identity keys, threshold release approval, reproducibility verification, and input policy. |
| [11](11-secrets-and-encryption.md) | Secrets and encryption | Encrypted subtrees, password and key protection, TPM sealing, and dedup-preserving encryption. |
| [12](12-search.md) | Search | Local BM25F and positional-trigram search over the working tree and named refs is implemented; published-artifact search follows the format registry. |
| [13](13-deployment-runtime.md) | Deployment runtime | The minimal Kubernetes distribution and the Railway-style developer experience. |
| [14](14-hot-update.md) | Hot update | Updating a running system from an incremental build, with honest limits. |
| [15](15-consumer-sync-and-web.md) | Consumer sync and web | The drive client, concurrent offline history, and Linux desktop state, hydration, offline, and badge integration are implemented; native Windows/macOS and web surfaces follow. |
| [16](16-universality.md) | Universality | Every language, platform and operating system, declared through the generated support matrix. |
| [17](17-implementation.md) | Implementation | C++26, the component inventory, and the build order. |
| [18](18-lake-service.md) | **lake.live** | The hosted service: GitHub + Google Drive + Railway over one store. |
| [19](19-identity.md) | Identity and auth | Clerk-quality auth, and why identity is the trust root for the whole chain of custody. |
| [20](20-secrets-service-and-pki.md) | Secrets service and PKI | Dynamic secrets, encryption as a service, and certificate issuance. |
| [21](21-liquify-ide.md) | **liquify** | The IDE. No language server, because the editor and the build are one graph. |
| [22](22-change-capture.md) | Change capture | How a write becomes a drip, and everything a drip sets in motion. |
| [23](23-metis-and-zetta.md) | Metis and Zetta | How liquid fits Orama's pivot — the data-availability mapping, and the Zetta/liquid storage overlap. |
| [24](24-multiplayer-and-agents.md) | Multiplayer and agents | Write flow control, private replicas, presence, leases, epochs, and per-line attribution are implemented; suspicious-region detection remains. |
| [25](25-identity-attestation-and-transparency.md) | Identity, attestation and transparency | TAAP/1 identity for every party, signed drips, the append-only log with optional public anchoring, and policy-pinned admission verification. |

## Status

The implemented Phase 1 chunk store selects a coroutine-native persistence backend. Disk is the
reference implementation, and the deterministic simulator also runs against an in-memory test
implementation with injected latency and failures.

This tree is the specification. Each document states implementation status where code has landed;
unimplemented behavior remains explicit, and open decisions stay in each document's final section.
The consolidated record of what is implemented, by phase, is
[`../library/status.md`](../library/status.md).
