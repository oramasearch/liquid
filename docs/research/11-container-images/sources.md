<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Container image build and distribution internals, 2024-2026

**Scope:** state of the ecosystem as of September 2026. The current stable OCI Image and Distribution specifications are both **v1.1.1**; the important artifact/referrers and zstd capabilities arrived in v1.1.0 during 2024. [web:287][web:293][web:295]

## 1. OCI image and distribution internals

### 1.1 The image is a Merkle-like graph of descriptors and blobs

| Component | Internal role |
|---|---|
| **Image index** | Optional JSON object whose `manifests[]` descriptors point to platform-specific image manifests. This is what a multi-platform tag normally resolves to. |
| **Image manifest** | JSON document containing one `config` descriptor and an ordered `layers[]` array. Each descriptor includes `mediaType`, byte `size`, and a digest. |
| **Config blob** | JSON containing runtime configuration-entrypoint, command, environment, working directory, user, exposed ports-and root-filesystem metadata such as ordered uncompressed layer `diff_ids`. |
| **Layer blob** | A filesystem changeset, normally a tar archive compressed with gzip or zstd. Added and modified paths are stored in full; removals are encoded as whiteouts. |
| **Descriptor digest** | Usually `sha256:<hex>` over the exact bytes of the referenced object. Consequently, the manifest digest depends on config and layer descriptors, while changing a compressed layer changes that layer's descriptor and therefore the manifest. |
| **Tag** | A mutable registry name pointing to a manifest. A digest reference is immutable in the content-addressing sense; a tag is not. |

OCI defines an image as a manifest, optional index, filesystem layers, and configuration. Registry clients obtain the manifest, choose a platform from an index if necessary, then fetch the config and layer blobs by digest. [web:299][web:229]

Two hashes are easy to confuse:

- A layer descriptor's `digest` addresses the **stored compressed blob**, such as the exact gzip bytes.
- The config's `rootfs.diff_ids[]` identifies the **uncompressed tar stream**. It lets a runtime verify the filesystem changeset after decompression and construct the ordered root filesystem.
- Because compression parameters can change compressed bytes without changing the uncompressed tar, the compressed digest and DiffID are intentionally different identities.

### 1.2 Layers: tar, gzip, zstd and whiteouts

The standard media types include:

| Layer representation | OCI media type | Consequence |
|---|---|---|
| Uncompressed tar | `application/vnd.oci.image.layer.v1.tar` | Direct extraction, but poor transfer size. |
| gzip-compressed tar | `application/vnd.oci.image.layer.v1.tar+gzip` | Traditional, broadly interoperable representation. |
| zstd-compressed tar | `application/vnd.oci.image.layer.v1.tar+zstd` | Added to OCI Image Spec 1.1; generally better compression and lower CPU cost, but requires end-to-end runtime/tool support. |

OCI 1.1 formally added zstd layer media types; OCI describes zstd as typically producing smaller output with less CPU than traditional gzip. [web:34][web:39]

A layer is not a complete directory tree but a changeset relative to its parents:

- Added or modified files are included in their entirety.
- Deletion of `foo` is represented by `.wh.foo`.
- `.wh..wh..opq` marks a directory opaque, hiding children from lower layers.
- Applying layers in manifest order produces the final root filesystem. [web:98]

This explains a notorious size trap: adding a 1 GB file in one layer and deleting it in a later layer does **not** remove its bytes from the image. The later layer only adds a whiteout; both blobs remain distributable.

### 1.3 Why tar layer blobs are a poor content-addressed storage format

| Problem | Why it happens | Practical result |
|---|---|---|
| **No native file/chunk-level deduplication** | The registry hashes and stores the whole compressed tar blob. A one-byte file change generally produces a new blob digest. | Two otherwise identical layers cannot share unchanged files unless their complete compressed blobs are identical. |
| **Duplicate bytes across layers** | A modified file is emitted in full in a later changeset; an old version remains in the lower layer. | Replacing a large package can ship both versions. |
| **Whiteout bloat** | Deletion is represented by metadata in an upper layer rather than removal from an immutable lower blob. | "Install in one `RUN`, delete in another" retains the installation bytes. |
| **Archive seriality** | Ordinary tar+gzip is a sequential stream without a random-access file index. | Traditional runtimes download and unpack entire blobs before accessing files. |
| **Unstable tar identity** | Tar headers include metadata such as timestamps, UID/GID, modes, PAX fields and names; directory enumeration determines member ordering. | Equivalent root filesystems can have different tar bytes and therefore different DiffIDs and image digests. |
| **Unstable compression identity** | gzip headers/options and compressor versions can vary. | Identical uncompressed tar streams can still have different compressed blob digests. |
| **Layer-boundary sensitivity** | Moving identical files between Dockerfile steps changes layer grouping and descriptors. | Semantically identical final filesystems can have unrelated image identities and poor registry reuse. |

GNU tar's own reproducibility guidance requires sorted names, normalized timestamps, removal of volatile atime/ctime fields, fixed ownership/locale, and `gzip -n`; these are not incidental details-they determine the content digest. [web:91] OCI explicitly specifies whole-file changes and whiteouts rather than block- or file-addressed storage. [web:98]

Thus OCI's CAS is excellent for **integrity, immutable references and exact-blob reuse**, but ordinary tar layers are a coarse deduplication unit.

### 1.4 Distribution Spec: "Registry API v2"

The OCI Distribution Spec standardized the push/pull protocol derived from Docker Registry HTTP API V2. A simplified pull is:

| Operation | Endpoint |
|---|---|
| Check registry | `GET /v2/` |
| Fetch index or manifest | `GET /v2/<name>/manifests/<tag-or-digest>` |
| Check blob availability | `HEAD /v2/<name>/blobs/<digest>` |
| Fetch config or layer | `GET /v2/<name>/blobs/<digest>` |
| Begin upload | `POST /v2/<name>/blobs/uploads/` |
| Complete upload | `PUT <upload-location>?digest=<digest>` |
| Publish manifest | `PUT /v2/<name>/manifests/<reference>` |

The registry validates uploaded content against the supplied digest. Existing blobs can be mounted or skipped, making distribution deduplicate **identical blobs** efficiently. [web:226][web:231]

A pull by tag therefore involves mutable name resolution followed by immutable digest-addressed downloads. Production deployment systems should record or pin the resolved manifest digest if they require repeatability.

### 1.5 OCI artifacts and the Referrers API

OCI 1.1 generalized an image manifest so registries can hold signatures, SBOMs, provenance, vulnerability reports, Helm-like packages and other objects:

| Field/mechanism | Purpose |
|---|---|
| `artifactType` | Identifies the semantic type of a non-container artifact. |
| `subject` | Descriptor of the image or artifact that this manifest describes. |
| Empty config | `application/vnd.oci.empty.v1+json` may satisfy the image-manifest shape when no meaningful runtime config exists. |
| Referrers endpoint | `GET /v2/<name>/referrers/<digest>` returns an OCI index of manifests whose `subject` matches that digest. |
| Filtering | Registries may filter by `artifactType`. |
| Compatibility fallback | On registries without the endpoint, clients may maintain a tagged index named from the subject digest, replacing `:` with `-`. |

Importantly, final OCI 1.1 did **not** retain a separate "artifact manifest" media type. It uses the existing image manifest plus `artifactType` and `subject`, because the proposed dedicated manifest caused portability concerns without enough benefit. [web:31]

The native API avoids altering the subject image's digest when attaching a signature or SBOM. The fallback tagged-index scheme works on older registries but is race-prone and clutters tags, so native registry support is preferable. [web:271][web:273]

### 1.6 Lazy-pull formats

| Technology | Representation and operation | Compatibility and trade-offs |
|---|---|---|
| **stargz / eStargz** | Reorganizes a gzip-compatible tar stream and supplies a table of contents so the snapshotter can range-fetch required chunks. eStargz adds verification, prefetch hints and workload-oriented optimization. | Still an OCI/Docker gzip layer and remains runnable by unaware runtimes, which simply pull it normally. Requires `stargz-snapshotter` for lazy operation; optimized file ordering may change the blob digest. |
| **stargz-snapshotter** | containerd remote snapshotter that mounts eStargz layers and fetches chunks when files are read. | Starts a container before full pull completion, but turns later file faults into registry/network I/O and needs registry byte-range support and local chunk caching. |
| **SOCI** | Builds a separate Seekable OCI index-containing zTOCs and span information-for an existing, unmodified OCI image. The SOCI snapshotter uses it to retrieve individual regions/files from the original compressed layers. | Does not require rewriting the original image, so its digest is preserved. The sidecar index must be generated, published and correctly associated. SOCI Index Manifest v2, introduced in 2025, strengthened the explicit image/index relationship and became the default AWS Fargate mechanism. [web:303][web:304][web:305] |
| **Nydus / RAFS** | Converts content into a chunk-addressed filesystem. File data is divided into chunks that are independently compressed, verified, cached and fetched; identical chunks can be deduplicated across layers and images. | Better fit for random access and true sub-layer deduplication, but it is an alternate image/filesystem representation requiring Nydus tooling and a Nydus-capable snapshotter/runtime path. It can use FUSE, virtiofs or in-kernel EROFS backends. [web:151][web:164] |

The architectural distinction is important:

- **eStargz** makes the layer itself seekable while retaining gzip compatibility.
- **SOCI** indexes an unchanged conventional image from the side.
- **Nydus** changes the storage model into a chunk-addressed filesystem.

Stargz's project definition of lazy pulling is precisely that the container can run before the image finishes downloading and required chunks are fetched on demand. [web:16]

Lazy pulling improves cold-start latency principally when startup touches only a fraction of a large image. It can be worse for workloads that immediately scan most files, for unreliable/high-latency registries, or where first-request latency matters more than container-process start time.

---

## 2. Dockerfile and BuildKit internals

### 2.1 LLB and the solver

BuildKit does not fundamentally execute a Dockerfile line-by-line. A frontend translates it into **LLB-Low-Level Build**, a content-addressed protobuf graph, analogous to an IR. Vertices represent source, file, execution, merge and related operations, connected through mounts and outputs. [web:46][web:263]

| BuildKit mechanism | Behavior |
|---|---|
| **DAG solver** | Walks only reachable dependencies, executes independent branches concurrently, deduplicates identical in-flight vertices and avoids unused stages. |
| **Vertex identity** | Derived from the operation definition and its input graph. Vertices with the same digest are treated as equivalent during a solve. [web:53] |
| **Cache records** | Map operation/input keys to immutable result snapshots and metadata. The result content can be stored locally or exported. |
| **Content-aware inputs** | `COPY`/file operations include selected source content and relevant metadata in their key; execution vertices also depend on input filesystem states, command, environment and mounts. |
| **Lazy context transfer** | BuildKit can request only files actually used by the graph rather than always sending and hashing everything eagerly. |
| **Parallel stage execution** | Independent multi-stage branches can execute concurrently; an unreferenced stage need not run. |

This graph model is more capable than the old builder, but a conventional Dockerfile still usually describes a mostly linear chain of filesystem states.

### 2.2 Why a change invalidates "everything after it"

For a linear sequence:

```dockerfile
COPY . .
RUN npm ci
RUN npm test
RUN npm run build
```

the input to each `RUN` is the output snapshot of the prior instruction. If `COPY . .` changes, the parent snapshot digest for every downstream operation changes. Even if the text `RUN npm ci` is identical, it is no longer the same operation over the same input state.

Docker's documented behavior is that when an instruction misses, later instructions are rebuilt; `COPY` and `ADD` compute checksums from copied content and relevant metadata, though file `mtime` alone is excluded. [web:166][web:168]

The common repair is:

```dockerfile
COPY package.json package-lock.json ./
RUN --mount=type=cache,target=/root/.npm npm ci
COPY . .
RUN npm run build
```

Now ordinary source edits do not invalidate dependency installation. A narrow `.dockerignore` is equally important: a broad `COPY . .` makes every included generated file, log, local dependency tree or permission change a possible invalidator.

### 2.3 Cache mounts versus layer cache

| Cache type | Semantics |
|---|---|
| **Immutable layer/result cache** | Reuses the complete result of an LLB vertex when its key matches. On a hit, the command does not execute. |
| **`RUN --mount=type=cache`** | Supplies a mutable persistent directory to an executing command-for example npm, Maven, Cargo, Go or apt downloads and compiler intermediates. Its contents are not committed to the image and should not affect correctness. |
| **Bind mount** | Makes source available to one `RUN` without necessarily copying it into the resulting layer. |
| **Secret/SSH mount** | Exposes credentials only to the instruction, without committing them to the layer or normal cache metadata. |
| **tmpfs mount** | Provides scratch memory-backed storage that disappears after the step. |

Cache mounts help after a layer-cache miss: the compiler or package manager still runs, but can reuse internal downloads and intermediate output. Docker describes them as cumulative across builds, so only new or changed packages need downloading. [web:268]

They are not guaranteed durable storage: garbage collection can remove them, sharing needs coordination (`shared`, `private`, or `locked`), and distributed CI may not export their mutable contents with ordinary layer cache. A real GitHub Actions report found the immutable cache partly restored but Cargo rebuilt because cache mounts were not exported. [web:5]

### 2.4 External cache exporters

| Exporter | Storage model | 2026 status/notes |
|---|---|---|
| `inline` | Embeds limited cache metadata in the output image configuration. | Simple; travels with the image, but tied to the image output and less suitable for rich multi-stage cache graphs. |
| `registry` | Publishes cache as a separate registry object/reference. | Usually the best portable CI option; `mode=max` can preserve intermediate-stage records. |
| `gha` | Uses GitHub Actions cache service. | Convenient for GitHub-hosted CI; subject to service scopes, quotas and eviction. |
| `local` | Writes an OCI-style cache directory. | Useful on persistent workers; awkward on ephemeral machines unless archived separately. |
| `s3` | Stores cache in an S3 bucket. | Still listed as **unreleased** in Docker's cache-backend documentation as of 2026. |
| `azblob` | Azure Blob-backed cache. | Also dependent on the selected BuildKit/buildx driver and deployment. |

External caches must be explicitly exported with `--cache-to` and imported with `--cache-from`; they do not appear automatically on a fresh CI runner. [web:47]

### 2.5 Multi-stage and multi-platform builds

| Feature | Benefit | Cost or pitfall |
|---|---|---|
| **Multi-stage build** | Build with compilers/toolchains in one stage, then `COPY --from` only runtime artifacts into a small final stage. Each `FROM` starts a separate stage. [web:169] | A careless cross-stage `COPY` can couple a large builder subtree to the final output and trigger broad rebuilds. |
| **Multi-platform index** | Produces platform-specific manifests under one index/tag. | Cache records and binaries are often platform-specific; all platforms multiply build and transfer work. |
| **QEMU/binfmt emulation** | Runs target-architecture commands on a different host architecture with minimal Dockerfile change. | Dynamic instruction translation is much slower, especially for compilation and compression. Docker explicitly recommends native nodes or cross-compilation for compute-heavy work. [web:50] |
| **Native builder cluster** | Builds each target on matching hardware. | Fastest execution, but requires multiple maintained workers and shared remote cache. |
| **Cross-compilation** | Compile on `$BUILDPLATFORM` for `$TARGETPLATFORM`, avoiding emulated compilers. | Language/toolchain must support cross-compilation; target-specific tests may still need native execution or emulation. |

---

## 3. Alternative builders and image strategies

| Tool/approach | Build model | Strengths | Limitations/status in 2026 |
|---|---|---|---|
| **Kaniko** | Executes Dockerfile commands in userspace inside a container/Kubernetes pod, snapshots filesystem differences and pushes layers without a Docker daemon. | Historically easy for daemonless Kubernetes CI. | Google's repository was archived on **June 3, 2025** and is explicitly no longer maintained. A Chainguard maintenance fork exists, but says no major feature work is planned; the original images should not be selected for new platforms. [web:197][web:207] |
| **Buildah** | Daemonless CLI/library that builds OCI images from Containerfiles or imperative commands using containers/storage. | No long-running root daemon; supports rootless builds and direct OCI output. [web:137][web:138] | Rootless operation still needs user namespaces, subordinate UID/GID mappings and often `fuse-overlayfs`; nested/Kubernetes security contexts can be difficult. |
| **ko** | Runs `go build`, assembles the Go binary and configured files over a base, then publishes directly. | Very fast and simple for Go; no Docker daemon or Dockerfile; good multi-platform support. [web:245][web:255] | Purpose-built for Go applications, not arbitrary shell-driven OS assembly. |
| **Jib** | Maven/Gradle/Java-library builder that constructs OCI layers directly. Separates dependencies, resources and classes. | No Dockerfile or daemon; fine-grained Java-aware cache reuse; reproducible output for identical contents. [web:254] | Java-centric; arbitrary package installation and shell customization are deliberately not its strength. |
| **melange** | Declarative pipelines build APK packages. | Makes application and OS packages explicit, versionable and testable. [web:68] | Requires adopting APK/Wolfi-style packaging rather than arbitrary Dockerfile mutation. |
| **apko** | Declaratively assembles APK packages into a usually single-layer OCI image. | Minimal, lockable, auditable and reproducible assembly; does not run arbitrary `RUN` commands. [web:61][web:62] | Custom software normally must first become an APK, commonly through melange. |
| **nix2container** | Maps Nix derivations/store paths into OCI layers and pushes them without first writing every tarball into the Nix store. | Hermetic dependency graph, reproducible inputs and ability to skip already-pushed layers. [web:181] | Nix learning curve and ecosystem integration; final OCI interchange still ultimately uses layer blobs. |
| **Bazel `rules_oci`** | Constructs OCI layouts/images from Bazel targets and tar layers; uses Bazel's dependency graph, sandboxing and remote cache. | Precise incremental build graph, remote execution/cache, multi-platform images and direct push/load rules. | It intentionally avoids being a general shell Dockerfile executor; base OS/package creation often remains a separate concern. |
| **Bazel `rules_docker`** | Older Bazel Docker/container rules. | Large installed base. | Maintained only as needed; `rules_oci` is the recommended alternative. The repository was archived in 2023 and reopened in 2024 mainly for community maintenance/security work. [web:192][web:185] |
| **Distroless** | Not a builder: minimal runtime base images containing the application runtime but no package manager or normal shell. | Smaller runtime surface, fewer unnecessary packages, harder for an attacker to use ordinary shell tooling. [web:184] | Debugging is harder; default images have no shell, so exec-form entrypoints and external/debug tooling are required. [web:189] |

### Reproducibility remains an end-to-end property

Even a deterministic image assembler cannot repair nondeterministic inputs. Exact image reproduction requires controlling:

| Source of variation | Typical control |
|---|---|
| Mutable base tags | Pin base manifests by digest. |
| Unpinned package repositories | Lock package versions and repository snapshots. |
| File mtimes and image `created` time | Apply `SOURCE_DATE_EPOCH`, `--timestamp`, or timestamp rewriting. |
| Tar member order | Sort names in a stable locale. |
| UID/GID, usernames and modes | Normalize numeric ownership and permissions. |
| PAX, atime and ctime metadata | Delete or canonicalize volatile fields. |
| gzip headers/options | Disable filename/timestamp headers and pin compressor behavior. |
| Compiler/linker randomness | Use reproducible compiler flags, stable paths and deterministic archives. |
| Network-generated dependencies | Vendor or content-pin inputs. |
| Multi-platform scheduling | Pin toolchains and ensure architecture-specific outputs are independently deterministic. |

Buildah/Podman expose `--source-date-epoch` and `--rewrite-timestamp` specifically to produce deterministic layers and images. [web:103] Jib, apko/melange, Nix and Bazel reduce nondeterminism by replacing arbitrary mutation with a structured dependency graph, but package repositories, compilers and externally downloaded content still need pinning.

---

## 4. What developers hate about Docker builds

| Complaint | Underlying cause | Real-world evidence and mitigation |
|---|---|---|
| **Slow rebuilds** | A changed parent snapshot invalidates downstream operations; expensive dependency resolution or compilation runs again. | A 2025 vLLM issue reported a build stage taking about **40 minutes** and asked that docs/Python-only changes behave like overlaying changed files in seconds. [web:115] Split dependency manifests from source, use cache mounts and maintain language-native remote caches. |
| **CI cache misses** | Ephemeral runners start without local BuildKit state; an external cache was not exported/imported, has the wrong scope, was evicted, or lacks intermediate records. | One user reported an unchanged expensive `RUN` increasing CI from under 15 minutes to over 40 minutes. [web:10] Use registry cache with `mode=max`, stable cache references and persistent/native builders where practical. |
| **The `COPY . .` problem** | Every included source or metadata change alters the `COPY` input key; all dependent vertices then receive a different parent. | Docker's own docs state that a changed `COPY` invalidates the layer and all following layers. [web:168] Use `.dockerignore`, copy lockfiles first, and delay broad source copies. |
| **No automatic incremental compilation inside a layer** | A layer-cache hit skips the compiler completely; a miss normally starts from the parent snapshot, which lacks the prior command's uncommitted compiler work. | A Cargo discussion summarizes the binary outcome: unchanged dependencies yield a layer hit, but changed dependencies yield a complete rebuild unless intermediate state is externalized. [web:217] Mount compiler output and package caches explicitly, or compile outside the image with Bazel/Nix/ko/Jib. |
| **Huge images and layer bloat** | Build tools, package indexes and replaced/deleted files remain in immutable earlier layers. | Developers repeatedly observe that deleting files in a later `RUN` only hides them and does not reduce the earlier layer's size. [web:211][web:214] Use multi-stage builds and clean temporary content in the same instruction that creates it. |
| **Huge local build cache** | Old result snapshots, unused layers and mutable cache mounts accumulate independently of tagged images. | A 2024 complaint noted there was no simple built-in policy to retain only the latest cache for each image and resorted to scheduled pruning. [web:110] Configure BuildKit garbage collection and cache quotas. |
| **Secrets in layers/history** | `ARG`, `ENV`, copied credentials and files deleted only in later layers remain in image metadata or lower blobs. | Docker explicitly warns that build args and environment variables persist and directs users to secret or SSH mounts. [web:121][web:122] |
| **Daemon dependency** | Traditional `docker build` requires access to the Docker daemon, often via a root-equivalent socket or privileged Docker-in-Docker. | Buildah, ko, Jib, apko and Kaniko emerged largely to generate/push OCI content without that daemon. Buildah explicitly requires no runtime daemon. [web:137] |
| **Rootless friction** | User namespaces, subordinate IDs, filesystem restrictions, nested mounts, networking and OverlayFS support vary across kernels and CI environments. | A 2024 Buildx issue reported `overlay2/.../work: permission denied`; rootless Buildah instructions require subuid/subgid ranges plus tools such as `fuse-overlayfs` and `slirp4netns`. [web:114][web:136] |
| **Multi-architecture builds are painfully slow** | QEMU translates target instructions and system calls while compilers perform CPU-heavy work. | Docker acknowledges that emulation is much slower for compilation and compression. [web:50] Build on native nodes or cross-compile in a `$BUILDPLATFORM` stage. |
| **Cache behavior feels opaque** | Keys include parent states, selected files, metadata, frontend versions, arguments, mounts and platform; cache export modes preserve different subsets. | A BuildKit user complained that even the first step performed a roughly 70-second image-config resolution on repeated builds despite expecting a cache hit. [web:1] Use plain progress logs, stable pinned inputs and explicit cache topology. |

---

## 5. Runtime storage and incremental delivery

### 5.1 OverlayFS and conventional snapshotters

Containerd's default Linux snapshotter is `overlayfs`, analogous to Docker's `overlay2`. [web:76][web:80]

A conventional pull path is:

1. Download compressed blobs into containerd's content store.
2. Verify each blob digest.
3. Decompress and apply layer 0 into a committed snapshot.
4. Create the next snapshot from that parent and apply layer 1.
5. Repeat, then mount the immutable lower snapshots with a writable upper directory.

Containerd documents this active-snapshot -> apply diff -> commit -> use as next parent sequence. [web:82]

OverlayFS provides:

- **Exact-layer sharing:** containers and images that use the same unpacked lower snapshot share it.
- **Copy-on-write:** modifying a lower file copies it into the container's writable upper layer.
- **Whiteouts:** removing a lower file creates a whiteout in the upper directory; the lower bytes remain. The kernel documents both whiteouts and opaque directories. [web:83]

It does **not** inherently provide global content-defined deduplication. If the same 500 MB file appears in two different layer blobs or snapshots, ordinary registry/containerd/OverlayFS storage may retain both copies. Its sharing unit is principally the blob/snapshot/lower directory, not each file or block.

### 5.2 Snapshotter spectrum

| Runtime storage model | Pull/start behavior | Deduplication granularity |
|---|---|---|
| **OverlayFS snapshotter** | Downloads and unpacks layers before ordinary access; mounts lower directories plus writable upperdir. | Exact shared layer/snapshot. |
| **eStargz remote snapshotter** | Mounts seekable gzip layers; range-fetches chunks on page/file access and optionally prefetches likely startup files. | Blob cache and chunks within each eStargz layer; not fundamentally global file CAS. |
| **SOCI snapshotter** | Uses sidecar zTOCs to access an unchanged original OCI gzip image lazily. | Cached spans from original blobs; preserves existing image digest but is not primarily a cross-image deduper. |
| **Nydus snapshotter/RAFS** | Mounts metadata first and fetches verified content chunks on demand. | Chunk-level content addressing and cross-layer/cross-image deduplication. Nydus stores only one copy when multiple filesystem references use the same chunk. [web:154][web:164] |
| **EROFS-based snapshotter** | Uses EROFS-formatted immutable blobs and combines them with OverlayFS for active writes. | Read-only filesystem/blob sharing; effectiveness depends on image generation and backing storage. Containerd still described this snapshotter as experimental. [web:79] |

### 5.3 CAS-backed filesystems are the more natural incremental-delivery model

A file- or chunk-addressed filesystem separates three concerns that tar conflates:

1. **Namespace metadata:** path, ownership, mode, symlink and directory structure.
2. **Content identity:** hashes of individual files or fixed/content-defined chunks.
3. **Physical transport/storage:** packs or blobs containing many independently addressable chunks.

That permits:

- downloading only chunks touched during startup;
- reusing an unchanged library even if surrounding layer contents or file ordering changed;
- deduplicating across image boundaries;
- verifying each fetched chunk independently;
- garbage-collecting based on reachable filesystem metadata rather than opaque tar ancestry.

Nydus is the clearest production-oriented example in this set: RAFS uses chunks as the units of compression, downloading, integrity verification, caching and deduplication. [web:164] The trade-off is ecosystem complexity: builders must emit or convert to the format, registries must serve efficient range requests, nodes need the snapshotter/filesystem implementation, and runtime performance becomes dependent on the remote content path.

## Bottom line

The 2024-2026 ecosystem is still constrained by an interchange format optimized for **portable streaming archives and exact-blob CAS**, not fine-grained incremental software delivery. BuildKit substantially improves computation through an LLB DAG, content-addressed result cache, cache mounts and remote exporters, but the final image remains an ordered stack of coarse tar changesets.

The practical division of labor is therefore:

- **BuildKit, Bazel or Nix** for graph-level incremental computation;
- **multi-stage builds, Jib, ko or apko/melange** for smaller and more deterministic outputs;
- **registry cache exporters** for ephemeral CI;
- **eStargz or SOCI** when compatibility with conventional OCI images matters most;
- **Nydus/RAFS-style chunk CAS** when file-level reuse, lazy access and cross-image deduplication justify a specialized runtime path;
- **OCI 1.1 subjects and referrers** for attaching signatures, SBOMs and provenance without mutating the image digest.

## Citations

[1] https://github.com/moby/buildkit/issues/3672
[5] https://github.com/docker/build-push-action/issues/743
[10] https://forums.docker.com/t/docker-is-not-using-a-cached-run-layer-on-ci-machine/145785
[16] https://github.com/containerd/stargz-snapshotter
[31] https://oci-playground.github.io/specs-latest/specs/image/v1.1.0-rc2/oci-image-spec.pdf
[34] https://opencontainers.org/posts/blog/2024-03-13-image-and-distribution-1-1/
[39] https://github.com/opencontainers/image-spec/blob/main/layer.md
[46] https://github.com/moby/buildkit
[47] https://docs.docker.com/build/cache/backends/
[50] https://docs.docker.com/build/building/multi-platform/
[53] https://github.com/moby/buildkit/blob/master/docs/dev/solver.md
[61] https://github.com/chainguard-images
[62] https://edu.chainguard.dev/open-source/build-tools/apko/overview/
[68] https://edu.chainguard.dev/open-source/build-tools/melange/overview/
[76] https://containerd.io/docs/2.2/snapshotters/readme/
[79] https://github.com/containerd/containerd/blob/main/docs/snapshotters/erofs.md
[80] https://docs.docker.com/engine/storage/containerd.md
[82] https://github.com/containerd/containerd/blob/main/docs/content-flow.md
[83] https://docs.kernel.org/filesystems/overlayfs.html
[91] https://www.gnu.org/software/tar/manual/html_section/Reproducibility.html
[98] https://github.com/opencontainers/image-spec/blob/main/layer.md
[103] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/10/html/building_running_and_managing_containers/introduction-to-reproducible-container-builds
[110] https://forums.docker.com/t/huge-build-cache-handling/142672
[114] https://github.com/docker/buildx/issues/2219
[115] https://github.com/vllm-project/vllm/issues/23588
[121] https://docs.docker.com/reference/build-checks/secrets-used-in-arg-or-env/
[122] https://docs.docker.com/build/building/secrets/
[136] https://www.akamai.com/docs/guides/using-buildah-oci-images/
[137] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/8/html/building_running_and_managing_containers/assembly_building-container-images-with-buildah
[138] https://buildah.io/
[151] https://github.com/containerd/nydus-snapshotter
[154] https://github.com/dragonflyoss/nydus
[164] https://nydus.dev/docs/concepts/rafs/
[166] https://docs.docker.com/build/cache/invalidation/
[168] https://docs.docker.com/build/cache/
[169] https://docs.docker.com/build/building/multi-stage/
[181] https://github.com/nlewo/nix2container
[184] https://github.com/GoogleContainerTools/distroless
[185] https://bazel.googlesource.com/bazel/+/93253eeae1b4ca4b1d24923fd4be49dd7b3b3a4c
[189] https://github.com/GoogleContainerTools/distroless/blob/main/README.md
[192] https://github.com/bazelbuild/rules_docker/blob/master/README.md
[197] https://github.com/GoogleContainerTools/kaniko
[207] https://github.com/chainguard-forks/kaniko
[211] https://forums.docker.com/t/why-run-command-which-deletes-files-inflates-image-size/33670
[214] https://forums.docker.com/t/docker-image-size-doesnt-make-sense/88424
[217] https://github.com/LukeMathWalker/cargo-chef/issues/151
[226] https://oci-playground.github.io/specs-latest/specs/distribution/v1.1.0-rc2/oci-distribution-spec.html
[229] https://docs.docker.com/reference/api/registry/latest.yaml
[231] https://pkg.go.dev/github.com/docker/distribution/registry/api/v2
[245] https://github.com/google/ko/pkgs/container/ko
[254] https://github.com/googlecontainertools/jib
[255] https://ko.build/reference/ko_build/
[263] https://github.com/moby/buildkit/blob/master/docs/dev/README.md
[268] https://docs.docker.com/build/cache/optimize/
[271] https://opencontainers.org/posts/blog/2024-03-13-image-and-distribution-1-1/
[273] https://oras.land/docs/concepts/reftypes/
[287] https://opencontainers.org/release-notices/overview/
[293] https://opencontainers.org/release-notices/v1-1-1-image-spec/
[295] https://github.com/opencontainers/distribution-spec
[299] https://github.com/opencontainers/image-spec/blob/main/spec.md
[303] https://github.com/sondavidb/soci-snapshotter
[304] https://aws.amazon.com/blogs/containers/improving-amazon-ecs-deployment-consistency-with-soci-index-manifest-v2/
[305] https://aws.amazon.com/about-aws/whats-new/2025/07/aws-fargate-soci-index-manifest-v2-deployment-consistency/
