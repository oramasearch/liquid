# liquid

**A version control system, build system, package manager, artifact service and
runtime — in one program.**

The name is the thesis: *software changes form*. Source becomes objects, objects
become libraries, libraries become packages, packages become images, images become
running processes. liquid moves a project along that chain in either direction,
computing only the minimum delta at each step, distributing the work across many
machines, with a cryptographic chain of custody throughout.

It aims to subsume the role of git, GitHub, Hugging Face, Spack, Nix, `docker build`,
CMake, Autotools, ccache/sccache, remote build execution, an artifact registry, a code
search engine, a PaaS, Dropbox, Clerk and Vault.

**[lake.live](docs/features/18-lake-service.md)** is the hosted expression of all of
it — liquid is the tool, lake is where it collects. A mix of **GitHub, Google Drive
and Railway** over one content-addressed store, with identity, secrets and PKI.

> **Status: design.** Nothing is implemented yet. This repository currently holds
> research and specifications.

## Where this sits

liquid is an **Orama** project (`oramasearch/liquid`, private). It is the intended
go-to-market vehicle for **Metis**, the Enterprise Intelligence OS Orama is pivoting to
build, whose central claim is that intelligence is bounded by *data availability*.

liquid manufactures the two lower bands of that availability — Reach and Fidelity — as
a side effect of being a tool teams adopt for other reasons, while **Zetta** turns
available data into running instruments.

See **[docs/features/23-metis-and-zetta.md](docs/features/23-metis-and-zetta.md)** for
the mapping, and for the open decision about the overlap between liquid's storage layer
and Zetta's.

## Documentation

| | |
|---|---|
| **[docs/research/](docs/research/)** | What the world currently does, what works, what is broken, and what people actually complain about. 25 topics, each with a curated analysis and — for 23 of them — the web-grounded sources it was drawn from. |
| **[docs/features/](docs/features/)** | What liquid does. 23 feature areas, each traceable to the research. |

Start with **[docs/research/99-synthesis/](docs/research/99-synthesis/)** for the
argument in one place, then **[docs/features/](docs/features/)** for the
specifications.

## Names

**liquid** is the tool. **[lake](docs/features/18-lake-service.md)** is where it
collects — the hosted service at `lake.live`. **[liquify](docs/features/21-liquify-ide.md)**
is the IDE.

A **drip** is one captured write to disk — the atom of history, automatic and
continuous. A **change** is a named, reviewable aggregation of drips. Drips are the
record; changes are the narrative.

## Design axioms

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

## What it must do

- Version anything, including multi-terabyte binary assets, from day one.
- **Compose repositories** — nest, split, and merge two unrelated codebases with
  history intact. Git cannot do this; it is liquid's headline feature.
- Diff *inside* zip, HDF5, Parquet, SQLite and OOXML — not just their bytes.
- Build incrementally as you type, with a build cache and compiler cache built in.
- Resolve dependencies at Spack's level of expressiveness.
- Produce `.deb`, `.rpm`, `.apk`, OCI images and more from one build.
- Serve builds, with a transparency log and signed provenance.
- Store encrypted secrets without destroying deduplication.
- Search full text and code, including inside published artifacts.
- Deploy to a minimal Kubernetes distribution with a Railway-style experience.
- Present a Dropbox/Drive-style interface for non-developers, local and web.
- Review changes, not branches — so review survives a rebase and stacking is native.
- Host models and datasets with training provenance — the chunk store Hugging Face
  already migrated to, plus the attestation chain a model hub cannot offer.
- Give you an editor with no language server, because the editor and the build are
  the same incremental graph.
- Provide identity, a runtime secrets service, and PKI — one CA serving signing,
  mTLS, TLS and SSH, because keyless signing already *is* certificate issuance.
- Support every language, platform and operating system — with the tier of support
  stated honestly rather than implied.

## Implementation

Modern **C++26**, cross-platform.

- **Datastore** — [`homer6/automerge-cpp`](https://github.com/homer6/automerge-cpp),
  a from-scratch C++23 CRDT implementation, for the metadata and namespace layer.
- **Search** — [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp)
  (BM25F) for ranked full-text, plus a positional trigram index for code substring
  and regex search.

See **[docs/features/17-implementation.md](docs/features/17-implementation.md)**.
