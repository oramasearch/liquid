# README reconciliation and historical context

Issue #284 replaces the duplicated front door with a build and first-result path. The source
snapshot was [`e5db43f2`'s README](https://github.com/oramasearch/liquid/blob/e5db43f2dd33a29868d7dcddc93600e47dd03236/README.md):
951 lines and 10,766 words. Its first “Build and run” heading was at line 509, followed by
another status inventory. The [implementation status](status.md) is now the single current
inventory; the [documentation index](../README.md) has one page row per topic.

## How competing passages were reconciled

The later copy was not consistently newer. Independent feature additions appeared on either
side. The following records explain what was retained and the evidence used to decide; linked
tests are the regression contracts, not a claim that every external integration ran for #284.

| Competing passages | Decision and evidence |
|---|---|
| Five adjacent Phase 1 announcements carried different hardware custody, flow-control, encryption, and delta additions. | Keep one Phase 1 inventory in [status](status.md#phase-1-the-store), retaining all those additions. The disk/memory backend, write-through, deltas, and repack behavior are implemented under [store](../../src/store/) and covered by [store tests](../../tests/test_store.cpp) and [collector tests](../../tests/store/), with the injected memory backend exercised by [simulator tests](../../tests/sim/); [identity keys](../../src/identity/keys/), [custody](../../src/identity/custody/), and [flow tests](../../tests/test_repository.cpp) cover the other variants. |
| “Compact/shallow” checkpoints competed with checked compact-or-full checkpoints. | Keep the latter: [repository persistence](../../src/repository/repository_impl.cpp) attempts compact save, verifies `load_sync`, and retries with `save_full`; [CRDT tests](../../tests/crdt/test_document.cpp) cover checkpoint behavior. |
| Short sync, drive, identity, admission, and frontend index rows omitted independent additions. | Merge peer limits and per-ref outcomes; pinning, offline state, clone and actor isolation; public identity export and hardware custody; authenticated checkpoint age; and multiplayer callbacks. The owning [sync](sync.md), [drive](drive.md), [identity](identity.md), [admission](admission.md), and [frontend](frontends.md) pages define the combined scope. |
| One Phase 4 copy included structural diff; the other included support tiers. | Retain both in status. [Structural diff tests](../../tests/build/test_structural_diff.cpp) cover edits and labelled fallback; [support-matrix tests](../../tests/test_tiers.cpp) cover registry entries and compiler preference. |
| Projection copies separately named native signing, Debian source packages, install generations, and OSTree/bootc. | Retain all alongside ecosystem artifacts. [Projection writers](../../src/projection/), [projection tests](../../tests/test_projection.cpp), and [install tests](../../tests/test_install.cpp) implement and exercise them. Keep the non-reproducible DMG qualification, Debian-only Tier B boundary, and unrecorded credentialed notarisation proof from [projection](projection.md). Projection alone does not establish boot support. |
| Configuration copies omitted either install intents or exclusive checkout rules; one inventory omitted Spack import. | Retain both language features and the import path. [Configuration tests](../../tests/config/), [lock CLI checks](../../tests/cli/locks.cmake), and [Spack import tests](../../tests/import/test_spack.cpp) are the contracts. Source declarations and canonical records remain covered by the existing [package guide](../guides/resolve-packages.md). |
| The original five format codecs competed with the expanded artifact-codec set. | Keep the union described by the [format registry](formats.md) and implemented in [format readers](../../src/formats/). |
| `CLAUDE.md` called REAPI transport and scheduling future work next to implemented announcements. | Retire the stale inventory; [REAPI client](../../src/reapi/), [worker implementation](../../src/workers/), and their [REAPI](../../tests/reapi/) and [worker](../../tests/workers/) tests support the current status entries. |
| “lake will link” competed with “lake links”; “Where this sits” still called the Zetta storage overlap an open decision. | Describe only the exported local library target, verified by the [consumer gate](../../tests/consumer/). External consumer/deployment state is not inferred from this checkout. The licence boundary is already decided in [repository decisions](../../CLAUDE.md#decisions--recorded): liquid owns its store and takes no dependency on Zetta or lake. |
| “Every write” and “nothing materialised until read” appeared as current behavior. | Keep the current introduction scoped to captured drips and the exercised repository workflow. [The first-repository guide](../guides/first-repository.md) starts daemon capture explicitly; [VFS](vfs.md) distinguishes full materialisation from lazy mounts. Broader promises are historical design requirements below. |
| Two build examples assumed an installed `liquid` and used eight jobs; the actual source build appeared near the end. | Put the native CMake bootstrap first, cap compilation at six jobs, and use the just-built CLI for init/cut/log. Wrapper, install, and consumer details stay in the existing guides and library/build pages. |

Names, specifications, and research already have maintained homes in the
[feature index](../features/README.md) and [research synthesis](../research/99-synthesis/README.md).
The former “Where this sits” discussion is retained as historical project context in
[the liquid/Metis/Zetta mapping](../features/23-metis-and-zetta.md); its withdrawn storage
recommendation is not a current open decision. No live-service deployment claim is made here.

## Historical design text

The following is one copy of the earlier README's design axioms and product requirements,
retained as historical intent. **These are not an implementation status or a support guarantee.**
In particular, universal offline operation, automatic capture of every write, universal lazy
materialisation, and default public-log anchoring must not be inferred from this archival text.
Use [status](status.md) and the relevant implementation page for current behavior and limits.

### Earlier design axioms

1. **One content-addressed chunk store.** Source, binaries, packages, images, search
   indexes and encrypted data are all chunks in one store. There is no "large file
   mode" because there is no distinction to make.
2. **One dependency graph of pure actions.** An action is a pure function from an
   input Merkle tree to outputs, addressed by digest.
3. **Dependencies are observed, never declared.** The sandbox records what an action
   actually read.
4. **Outputs are content-addressed.** A rebuild producing identical bytes stops the
   cascade. This is what kills "build the world".
5. **Nothing is materialised until it is read.**
6. **Reproducibility is enforced, not aspirational.** It is load-bearing for caching,
   substitution and provenance.
7. **Provenance is a byproduct.** Every action already records everything an
   attestation needs.
8. **Everything works offline**, degraded but correct.
9. **Every write to disk is a versioned change.** Drip, drip, drip.
10. **Every change is attributable.** [TAAP/1](../standards/taap-1-spec-draft.md) is
    the security model: every claim liquid makes about a user, an agent, a document, a
    build or an artifact is a signed attestation, and history is an append-only,
    tamper-evident log with a witness quorum — optionally anchored to a public chain. On
    by default.

### Earlier product requirements

- Version anything, including multi-terabyte binary assets, from day one.
- **Compose repositories** — nest, split, and merge two unrelated codebases with
  history intact. Git cannot do this; it is liquid's headline feature.
- Diff *inside* zip, HDF5, Parquet, SQLite and OOXML — not just their bytes.
- Build incrementally as you type, with a build cache and compiler cache built in.
- Resolve dependencies at Spack's level of expressiveness.
- Produce `.deb`, `.rpm`, `.apk`, OCI images and Helm charts from one build.
- Serve builds, with a transparency log and signed provenance.
- Store encrypted secrets without destroying deduplication.
- Search full text and code, including inside published artifacts.
- Deploy to a minimal Kubernetes distribution — a private, upstream-synced copy of
  [k0s](https://github.com/oramasearch/k0s) modified for drips — with a Railway-style
  experience.
- Present a Dropbox/Drive-style interface for non-developers, local and web.
- Review changes, not branches — so review survives a rebase and stacking is native.
- Host models and datasets with training provenance — the chunk store Hugging Face
  already migrated to, plus the attestation chain a model hub cannot offer.
- Give you an editor with no language server, because the editor and the build are
  the same incremental graph.
- Attribute everything — users, agents, documents, builds, artifacts — with signed
  [TAAP/1](../standards/taap-1-spec-draft.md) attestations, in an append-only log with
  independent witnesses, optionally anchored to a public chain for timestamping that does
  not require trusting us.
- Provide identity, a runtime secrets service, and PKI — one CA serving signing,
  mTLS, TLS and SSH, because keyless signing already *is* certificate issuance.
- Support every language, platform and operating system — with the tier of support
  stated honestly rather than implied.

