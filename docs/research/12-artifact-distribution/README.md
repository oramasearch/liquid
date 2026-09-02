# Artifact distribution and content-addressed storage

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid is also an artifact service that serves builds.

## Three architectures people confuse

1. **Package repositories** (Artifactory, Nexus, GitHub/GitLab Packages, CodeArtifact,
   Cloudsmith) — format-aware: they understand maven coordinates, npm tarballs,
   PyPI wheels, Debian pools. They serve *named versions* to *ecosystem clients*.
2. **Content-addressed stores** (Bazel CAS, Nix store, IPFS, restic/borg/kopia,
   casync, ostree) — name-free: they serve *bytes by hash*. Deduplication and
   integrity are intrinsic; discovery is somebody else's problem.
3. **OCI registries** — increasingly the universal substrate for both, via ORAS and
   OCI 1.1's `artifactType`/`subject`.

liquid needs all three roles, and the important insight is that **(2) is the
storage layer and (1) is a view over it**. Building (1) on top of (2) is
straightforward; the reverse is not.

## What users complain about in artifact repositories

Cost and storage bloat above everything; poor deduplication (the same dependency
stored many times across repositories and formats); slowness at scale; retention
and cleanup policies that are hard to reason about and dangerous to get wrong; and
metadata sprawl. The common root cause: these systems store **whole files keyed by
name**, so they cannot dedup across versions of the same artifact, let alone across
formats.

## Chunking and delta transfer: the toolkit

- **Content-defined chunking** — Rabin, Gear, **FastCDC**. Covered in depth in
  [02-large-files-and-binary-assets](../02-large-files-and-binary-assets/);
  FastCDC is the right choice.
- **Merkle DAGs** — Git's trees, IPFS/IPLD's UnixFS, Bazel's `Directory` protos.
  All the same idea; the differences are in chunking strategy and metadata.
- **casync/desync** — serialises a directory tree into a stream, chunks it, stores
  chunks in a store, and ships a small index. Notably it removes file boundaries
  *before* chunking, so small files group and large files split naturally. Designed
  exactly for shipping OS images efficiently.
- **ostree** — checks out by hardlinking from a content-addressed store. Atomic,
  rollback-capable, deduplicated across versions.
- **Delta transfer** — rsync's rolling checksum; zsync's server-prepared map;
  bsdiff (excellent patches, catastrophic memory); xdelta3 (windowed, streams);
  **zstd `--patch-from`** (fast, good enough, already a dependency);
  Courgette/Zucchini (executable-aware, expensive to generate, tiny patches).
- **Android A/B OTA** — the production example of generate-once/apply-many:
  50–200 MB delta versus 2–4 GB full payload.

## Lazy and partial fetch

The recurring conclusion across four different domains:

- Bazel's **Build without the Bytes** — don't download intermediates
  ([07](../07-distributed-execution/)).
- **EdenFS / VFS for Git** — don't materialise source files until read
  ([16](../16-monorepo-scale-and-authoring-loop/)).
- **stargz / SOCI / Nydus** — don't pull image layers until a file is touched
  ([11](../11-container-images/)).
- HTTP **range requests** over a CDN — the transport that makes all of the above
  work.

Four communities independently discovered that eager materialisation is the
dominant cost. liquid should treat lazy fetch as the default posture everywhere,
served by one mechanism.

## Garbage collection: the genuinely hard part

This is where content-addressed stores get operationally painful, and it is
under-appreciated.

- **Reference counting** is simple and breaks on cycles and on concurrent writers.
- **Mark and sweep** requires enumerating all roots, which in a distributed shared
  cache means knowing every client's live set — and a client that goes offline
  mid-build can have its inputs collected underneath it.
- **Nix GC roots** are explicit filesystem symlinks; simple, and it works because
  the store is machine-local.
- **Bazel cache eviction** is typically LRU with a size cap, which produces the
  **AC/CAS skew** failure: an Action Cache entry survives while the CAS blobs it
  references are evicted, so a "hit" cannot be materialised
  ([06](../06-build-and-compiler-caching/)).
- **Chunked stores make this worse**, because a single logical artifact is
  thousands of chunks, any one of which being collected corrupts it.

Correct GC in a shared, distributed, chunked store is a research-grade problem and
must be designed in from the start, not added later.

## Implications for liquid

1. **One store, many views.** A single content-defined-chunk CAS underneath;
   package-repository semantics (npm, PyPI, Maven, apt, OCI) as *views* that map
   names and versions onto content hashes. This is what fixes the dedup complaint:
   the same vendored library inside a wheel, a `.deb` and an OCI layer is stored
   once, because liquid decomposes containers
   ([03](../03-structural-diff-and-formats/)) before storing them.

2. **Speak OCI distribution as the primary wire protocol.** It is universally
   deployed, CDN-friendly, and OCI 1.1's `subject`/Referrers gives attestation
   attachment for free ([13](../13-supply-chain-security/)). Speak REAPI's CAS for
   build traffic ([07](../07-distributed-execution/)). Do not invent a third
   protocol unless measurement forces it.

3. **Serve chunks, not files.** Range requests over packed blocks, with the
   dedup unit (chunks, ~64 KiB) distinct from the transfer/storage unit (packed
   blocks, ~tens of MiB) as Xet does. Naive per-chunk objects will die of request
   overhead.

4. **Design GC before launch.** Concretely: explicit, leased roots with expiry
   (so an abandoned client's roots age out rather than pinning forever); generation
   or epoch-based marking so collection does not require a global stop; blob
   lifetime tied to Action Cache entry lifetime so skew is impossible; and
   chunk-level reference tracking that is cheap enough for billions of objects.
   Getting this wrong shows up as either unbounded storage cost or corrupted
   artifacts, and both are fatal to an artifact service.

5. **Delta transfer as an optimisation on top of chunking, not instead of it.**
   Chunking handles the general case with no per-pair work. Reserve `zstd
   --patch-from` and executable-aware delta for the high-value
   generate-once/apply-many cases — published releases, OTA-style updates — where
   the asymmetric cost pays for itself.

6. **Transparency log at the distribution layer.** Every published artifact
   recorded in an append-only Merkle log, Go-checksum-database style. This is what
   makes "serve builds" trustworthy rather than merely convenient
   ([13](../13-supply-chain-security/)).

7. **Retention policy must be comprehensible.** The universal complaint about
   existing registries. Policy should be expressible in terms users think in
   ("keep every release tag forever; keep CI artifacts 30 days; keep anything
   referenced by a retained artifact"), with reachability computed by the tool.

8. **Serve the search index from the same store.** Since liquid indexes content by
   chunk and blob ([19](../19-full-text-search/)), searching what the artifact
   service holds is the same infrastructure, not a bolt-on.
