# Container images

> Evidence: [`sources.md`](sources.md)

## What OCI gets right

An image is a Merkle-ish graph of descriptors and blobs: an optional index points
at platform-specific manifests; a manifest holds a config descriptor and an ordered
layer list; every descriptor carries a media type, size and digest. Reference by
digest is immutable and verifiable. Registries deduplicate identical blobs and can
mount them across repositories. The distribution protocol is simple, universally
implemented, and CDN-friendly.

**OCI 1.1 (2024) added two genuinely important things:** zstd layer media types,
and the `subject` field plus the **Referrers API**, which lets signatures, SBOMs,
provenance and scan results be attached to an image *without changing its digest*.
That last property is what makes attestation practical
([13](../13-supply-chain-security/)). Note that the proposed separate "artifact
manifest" was **dropped** — OCI 1.1 uses an ordinary image manifest with
`artifactType` and `subject`.

## Why tar layers are the wrong content-addressed format

This is the central technical finding, and it matters because liquid is designing a
storage layer that must not repeat it.

- **No file- or chunk-level dedup.** The registry hashes the whole compressed tar
  blob. A one-byte change to one file produces an entirely new blob.
- **Duplicate bytes across layers.** A modified file is re-emitted in full; the old
  version remains in the lower layer.
- **Whiteout bloat.** Deleting a file in a later layer adds a `.wh.` marker; the
  1 GB file you "removed" still ships. This is the single most common size
  surprise.
- **Archives are serial.** Ordinary tar+gzip has no random-access index, so
  runtimes download and unpack whole blobs before touching a file.
- **Tar identity is unstable.** Timestamps, UID/GID, modes, PAX fields and
  directory enumeration order all affect the bytes. Equivalent filesystems produce
  different digests. GNU tar's own reproducibility guidance — sort names, normalise
  timestamps, strip atime/ctime, fix ownership and locale, `gzip -n` — is not
  incidental advice; it *determines the content digest*.
- **Layer boundaries are load-bearing.** Moving identical files between Dockerfile
  steps changes layer grouping and therefore all downstream digests.

OCI's CAS is excellent for **integrity and exact-blob reuse**, and poor as a
**deduplication unit**.

## BuildKit is better than people give it credit for

BuildKit does not execute a Dockerfile line by line. A frontend lowers it into
**LLB**, a content-addressed protobuf DAG, and a solver walks only reachable
vertices, runs independent branches concurrently, and deduplicates identical
in-flight work. Vertex identity derives from the operation plus its input graph.

That is the same action-graph model as [04-build-systems](../04-build-systems/) —
BuildKit is a real build system. Its limitation is the *input language*: a
conventional Dockerfile describes a mostly linear chain of filesystem states, so
the graph the solver gets is a chain.

**Why one change invalidates everything after it** follows directly: each `RUN`'s
input is the previous instruction's output snapshot. Change `COPY . .` and every
downstream vertex has a different parent, even though the command text is
identical. The standard fix — copy lockfiles, install dependencies, *then* copy
source — is people manually reordering the graph because the language cannot
express the real dependencies.

**Cache mounts** (`RUN --mount=type=cache`) give a mutable directory that persists
across builds for package-manager and compiler caches. They are a partial answer to
"no incremental compilation inside a layer," but they are not durable, are not
exported with the layer cache by default, and a real GitHub Actions report found
Cargo rebuilding because cache mounts were not exported.

**Cache exporters** (`inline`, `registry`, `gha`, `local`, `s3`, `azblob`) must be
explicitly wired with `--cache-to`/`--cache-from`; nothing is automatic on a fresh
CI runner. `s3` was still listed as unreleased as of 2026.

## Lazy pulling: three different bets

| Approach | Mechanism | Trade-off |
|---|---|---|
| **eStargz** | Makes the layer itself seekable while staying gzip-compatible; adds a TOC and prefetch hints | Runnable by unaware runtimes; but optimised file ordering **changes the blob digest** |
| **SOCI** | Builds a *sidecar* index (zTOCs) for an unmodified image | **Preserves the original digest** — the key advantage. Index must be generated and associated. Index Manifest v2 (2025) is the AWS Fargate default |
| **Nydus / RAFS** | Replaces the storage model with a chunk-addressed filesystem; chunks are the unit of compression, fetch, verification, caching and dedup | True sub-layer and cross-image dedup. Requires its own tooling and snapshotter |

**Nydus is the one that matches liquid's model**, because it reaches the same
conclusion from the runtime side that content-defined chunking reaches from the
storage side ([02](../02-large-files-and-binary-assets/)): separate namespace
metadata from content identity from physical transport, and everything else gets
better.

## What developers hate

Slow rebuilds (a 2025 vLLM issue reported a 40-minute stage for a docs-only
change); CI cache misses (an unchanged `RUN` taking a build from 15 to 40+
minutes); the `COPY . .` invalidation cliff; no incremental compilation inside a
layer; image bloat from deleted-but-still-present files; unbounded local build
cache with no good retention policy; secrets persisting in `ARG`/`ENV` and layer
history; the daemon dependency; rootless friction (user namespaces, subuid/subgid,
`fuse-overlayfs`); QEMU emulation making multi-arch builds punishingly slow; and
opaque cache behaviour.

## The builder landscape

**Kaniko was archived by Google on 3 June 2025** — an important operational fact,
since it remains widely deployed. A Chainguard maintenance fork exists with no
planned feature work.

Buildah (daemonless, rootless), ko (Go), Jib (Java, no daemon, fine-grained layer
reuse), apko/melange (declarative APK assembly, usually single-layer, reproducible),
nix2container, and Bazel's `rules_oci` (`rules_docker` is superseded) all take the
same position from different angles: **replace arbitrary `RUN` mutation with a
structured dependency graph**, and reproducibility plus fine-grained reuse follow.

That is precisely liquid's thesis.

## Implications for liquid

1. **Do not adopt tar layers as liquid's internal representation.** liquid's store
   is content-defined chunks ([02](../02-large-files-and-binary-assets/)). OCI is
   an *export format* — an interoperability boundary, not the model.

2. **Emit OCI images as a projection, and make them deterministic by
   construction.** Sorted entries, normalised timestamps via `SOURCE_DATE_EPOCH`,
   fixed ownership and modes, stripped volatile metadata, pinned compressor
   settings. Since liquid controls the whole graph, the output should be bit-identical
   across machines by default rather than by careful configuration.

3. **Compute layers, don't let users hand-order them.** liquid knows the real
   dependency graph, so it can partition the filesystem into layers that maximise
   registry reuse — stable dependencies low, volatile application code high —
   automatically. This eliminates the entire `COPY . .` genre of problem, because
   the user is no longer manually encoding a dependency order in instruction
   sequence.

4. **Never ship deleted bytes.** Because liquid computes the final filesystem
   rather than replaying mutations, whiteout bloat simply cannot occur. Worth
   stating as a guarantee.

5. **Incremental compilation inside the image, for free.** The loudest Docker
   complaint. liquid compiles with its own cache
   ([06](../06-build-and-compiler-caching/)) and then assembles an image from the
   results — so a one-line source change recompiles one object and produces a new
   image with one changed chunk. No cache mounts, no `cargo-chef`, no dependency
   pre-copy trick.

6. **Use `subject` + Referrers for all attestations.** Signatures, SBOMs and
   provenance attach without mutating the image digest. Fall back to the tagged
   index scheme only for old registries.

7. **Support zstd and consider Nydus/SOCI output.** zstd for size and CPU. For lazy
   pulling, SOCI's digest-preserving sidecar is the pragmatic choice;
   Nydus/RAFS is the one philosophically aligned with liquid's chunk store and the
   natural target if liquid also owns the runtime
   ([20](../20-deployment-runtime-k8s/)).

8. **Secrets never enter a layer.** Build-time secrets are mounted, never copied,
   and never appear in history — connecting to
   [14-secrets-and-encryption](../14-secrets-and-encryption/).

9. **Cross-compile rather than emulate.** QEMU is the wrong default. liquid ships
   toolchains in the input tree ([07](../07-distributed-execution/)), so
   cross-compilation is the natural path, with native remote workers where a target
   genuinely must execute.
