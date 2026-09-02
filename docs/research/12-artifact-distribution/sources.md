<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Deep research: artifact repositories, CAS, and distribution systems, 2024–2026

## Executive summary

Three architectures increasingly overlap but should not be confused:

1. **Package registries** provide ecosystem-specific naming, versioning, indexes, access control, vulnerability metadata, and upstream proxying. Their dominant scaling problem is often not raw blob storage but retained versions, tags, indexes, database rows, and cleanup semantics.
2. **Content-addressable stores (CAS)** identify immutable bytes by digest and naturally deduplicate identical objects. They still need a separate graph or metadata layer to answer “which objects form release X?” and “which objects are live?”
3. **Delta and lazy-distribution systems** minimize transfer either by precomputing source-to-target patches, transferring missing chunks, or postponing downloads until files are actually read.

OCI 1.1, finalized in 2024, substantially strengthened OCI registries as general artifact stores through `artifactType`, `subject`, and the Referrers API. Nevertheless, OCI remains primarily a blob-and-manifest substrate—not a replacement for Maven/PyPI/npm repository semantics, arbitrary sub-file deduplication, or a build cache’s Action Cache.

---

# 1. Artifact registries

**Legend:** ✓ = native/direct support; OCI = stored through the OCI/container repository; Raw = arbitrary-file repository; — = not a native repository format. “Docker” includes the product’s container registry, even where operated separately from the language-package registry.

| Artifact registry | What it stores | Maven | npm | PyPI | Docker / OCI | deb | rpm | Helm | Generic | Retention / cleanup | Typical user complaints and operational caveats |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| **JFrog Artifactory** | Local, remote-proxy, virtual and federated repositories; packages, containers, files, build information and associated metadata. | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓, including Helm OCI | ✓ | Current cleanup policies support age, last-use and keep-latest-version conditions across many formats; smart archiving is also available in applicable editions. [web:1][web:2][web:395][web:396] | High license/operations cost and a substantial DB/filestore footprint are common concerns. Importantly, Artifactory itself has strong **whole-binary** deduplication: binaries are stored once by checksum while each repository path creates another metadata mapping. Thus “bloat” is commonly caused by retained unique builds, remote caches and metadata—not duplicate physical copies of identical files. [web:348][web:350][web:351] |
| **Sonatype Nexus Repository** | Hosted, proxy and group repositories covering package ecosystems, containers and raw files. | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ via Yum | ✓ | ✓ as **Raw** | Policies can select by component age, last download, release type and asset-name expression. Keep-latest is supported in narrower cases such as Maven releases and Docker, with edition/database/version constraints. Deletion and physical reclamation can be separate operations. [web:6][web:392][web:400] | Frequent operational complaints concern blob growth and surprising space reclamation: cleanup can soft-delete content, after which compaction is needed to return disk space. Reports also describe cleanup tasks failing to prevent substantial growth. [web:78][web:79][web:81] |
| **GitHub Packages / GHCR** | npm, RubyGems, Maven/Gradle, NuGet and container images. | ✓ | ✓ | — | ✓ | — | — | — | — | Individual packages and versions can be deleted through the UI/API. GitHub supplies a `delete-package-versions` Action for implementing keep-N or age-based automation, but there is no comparably rich repository-native lifecycle-policy engine. [web:46][web:54][web:362] | Limited format breadth, relatively low included private storage, storage shared with Actions artifacts, metered transfer, and the need to build retention workflows are recurring objections. GitHub’s published quotas range from 500 MB on Free to 50 GB on Enterprise Cloud, before overage billing. [web:82] |
| **GitLab Package Registry + Container Registry** | Maven, npm, NuGet, PyPI, generic and Terraform packages; Composer/Helm and several others at varying maturity; Docker/OCI images are held in the separate integrated Container Registry. | ✓ | ✓ | ✓ | ✓ | Experimental Debian support | — | Beta / OCI | ✓ | Container cleanup policies remove tags by age, regex and keep-N criteria. Removing tags does not immediately remove all underlying layers; self-managed installations may require registry GC, while the newer metadata database supports online GC. General Package Registry lifecycle policies remain less complete, so deletion is often API/CI driven. [web:48][web:50][web:52][web:59][web:355] | Users encounter combined project storage quotas, tag-versus-blob accounting confusion, and delayed reclamation. The container registry does deduplicate layers within the root namespace, but old tagged manifests and package versions still accumulate. [web:352] |
| **AWS CodeArtifact** | Cargo, generic, Maven, npm, NuGet, PyPI, RubyGems and Swift packages; upstream repository caches. It is not AWS’s container registry—ECR serves that role. | ✓ | ✓ | ✓ | — | — | — | — | ✓ | Versions can be archived, disposed or explicitly deleted, but CodeArtifact still lacks a native configurable lifecycle/retention policy; AWS publishes a Lambda/EventBridge workaround. [web:31][web:35][web:370] | Narrower format coverage, no containers/deb/rpm/Helm, and cost sensitivity in high-request dependency workloads. Charges have three dimensions: stored GB, requests—including assets fetched from public upstreams—and regional data transfer. [web:316][web:331] |
| **Google Artifact Registry** | Docker/OCI and OCI Helm charts, Maven, npm, Python, Apt/Debian, Yum/RPM, Go, generic artifacts and additional Google-oriented formats. Repositories are format-specific. | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | OCI | ✓ | Delete and keep policies can select artifacts by conditions or preserve a number of recent versions; policies are available across repository formats. [web:32][web:33] | Storage growth and billing when cleanup is not configured are common surprises; old builds are not simply assumed disposable. Users must also operate separate repositories per format/location. Storage and cross-location transfer are billed, though some same-location traffic is free. [web:318][web:334][web:376] |
| **Cloudsmith** | Hosted multi-format service for packages, Docker/OCI, raw binaries, firmware, models and datasets. | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ / Raw | Retention rules can group by name, format, type or query and impose limits by package count, age or total size. [web:44][web:398] | The principal complaint is price at scale, particularly because both artifact storage and delivery bandwidth are plan/usage dimensions; published plan material lists $1.50/GB overage in relevant tiers. [web:329][web:345] |

### What “multi-format” really means

A universal repository is more than a file server:

- **Maven** needs coordinates, POM metadata, snapshots and repository metadata.
- **npm/PyPI** need package indexes and client-specific authentication and metadata responses.
- **Deb/RPM** require signed indexes and repository metadata regeneration.
- **Docker/OCI** needs manifests, layer blobs, tags and digest-based pulls.
- **Helm** may mean classic chart repositories with an `index.yaml`, or OCI-packaged charts.
- **Generic/raw** generally supplies path/version storage and download APIs but no package-manager dependency semantics.

Consequently, storing a `.deb` as a generic blob is not equivalent to serving an Apt repository.

### Why storage still grows despite deduplication

1. **Every genuinely different compressed archive is a different blob.** A timestamp or reordered tar member changes the digest.
2. **Package-level dedup is usually whole-object or OCI-layer based**, not content-defined sub-file deduplication.
3. **Tags and versions remain roots.** Deleting one tag does not free a shared layer.
4. **Proxy caches retain upstream artifacts.**
5. **Metadata multiplies independently of blob bytes.** Artifactory, for example, stores one checksum-addressed binary but a DB mapping for every repository path. [web:346][web:351]
6. **Logical deletion may precede physical compaction or GC**, particularly in Nexus and container registries.
7. **Indexes and relationship graphs grow:** packages, versions, assets, manifests, tags, SBOMs, signatures, provenance and scans can produce many rows per deliverable.

---

# 2. OCI registries as universal artifact stores

## OCI as a general CAS

An OCI registry is fundamentally a **repository-scoped content-addressed blob service** plus small JSON graph objects:

```text
tag
  └── manifest/index digest
        ├── config descriptor → digest
        ├── layer/blob descriptor → digest
        └── subject descriptor → another manifest digest
```

A descriptor includes a media type, digest and size. Manifests and indexes name their child blobs by digest, while mutable tags resolve to immutable manifest digests. OCI’s image specification explicitly defines descriptors as content-addressed references. [web:16][web:21]

With ORAS, arbitrary files can be:

1. uploaded as OCI blobs;
2. described by an OCI image manifest;
3. assigned an `artifactType`;
4. tagged for human discovery; and
5. pulled and verified by digest.

That makes a conforming registry useful for Helm charts, WebAssembly modules, firmware, ML models, SBOMs, signatures, provenance and arbitrary release bundles—not only runnable container images. ORAS supports both registries and local OCI-layout directories. [web:21]

## OCI 1.1 Referrers API

OCI Image and Distribution 1.1 added three particularly important pieces:

- **`artifactType`** identifies non-container content without overloading the config media type.
- **`subject`** points from an attachment manifest to the digest of the artifact being described.
- **`GET /v2/<name>/referrers/<digest>`** returns an OCI index containing manifests that name that digest as their subject. [web:27][web:29]

This models:

```text
image@sha256:A
 ├── signature@sha256:B
 ├── SBOM@sha256:C
 ├── provenance@sha256:D
 └── vulnerability-report@sha256:E
```

Attachments do not mutate the signed image manifest, which is critical because changing that manifest would change its digest. OCI 1.1 was released in March 2024. [web:18]

## Strengths and limitations

| Property | OCI-registry result |
|---|---|
| Integrity and immutability | Strong when consumers pin and verify digests. Tags remain mutable aliases. |
| Whole-blob deduplication | Natural: identical digests can share storage, subject to registry tenancy/repository implementation. |
| Merkle graph | Manifests/indexes contain digest edges to blobs and other manifests. |
| Arbitrary artifact support | Good with ORAS and custom media types. |
| Related metadata | Stronger with OCI 1.1 `subject` and Referrers. |
| Sub-file deduplication | Not automatic. A 10-GB file uploaded as one layer changes entirely if one byte changes. The producer must chunk it into multiple blobs. |
| Package semantics | Weak compared with Maven/npm/PyPI registries: OCI does not define dependency resolution, ecosystem indexes or version ordering. |
| Cross-repository dedup | Implementation-dependent; authorization boundaries can prevent global mounting or reuse. |
| GC | Harder after adding referrers: registries must determine whether a manifest, attachment or blob remains reachable through tags, subjects and indexes. |
| Distribution | Excellent ecosystem support for digest pulls, CDN caching and replication, but many clients still assume container-oriented manifests and media types. |

**Practical conclusion:** OCI is a good universal **transport and CAS envelope**. It is not automatically a universal package manager or a fine-grained deduplicating store.

---

# 3. Content-addressable storage and Merkle-DAG designs

A true CAS computes something resembling:

```text
id = H(canonical_bytes)
put(id, bytes)
get(id) → bytes
```

A Merkle DAG extends this by placing child digests inside parent objects. Changing a leaf changes its digest and every ancestor digest up to the root, giving an immutable snapshot identity.

| System | Addressing and graph design | Chunking / compression / deduplication | Important consequences |
|---|---|---|---|
| **Git** | Blobs, trees, commits and annotated tags are immutable hash-addressed objects. Trees and commits form a Merkle DAG. | Loose objects are individually compressed. During repacking, Git groups objects by type, size and optionally pathname and stores either complete compressed objects or `OBJ_OFS_DELTA`/`OBJ_REF_DELTA` representations against base objects. Delta-chain depth trades size for reconstruction cost. [web:121][web:124] | Git deduplicates identical whole blobs, but ordinarily does not CDC-chunk a large file. Pack deltas are a storage/transfer representation; the logical object ID still identifies the complete canonical object. |
| **IPFS / IPLD / UnixFS** | IPLD links are CIDs containing content identifiers; UnixFS represents files and directories as Merkle DAGs. | UnixFS supports fixed-size and Rabin content-defined chunking, with balanced and trickle layouts. A balanced DAG creates internal nodes that link to bounded groups of chunks. [web:126][web:130] | Fixed chunks are fast but suffer boundary shifting after insertion. Rabin CDC preserves more chunk identities. DAG fan-out controls random access and traversal overhead. |
| **Perkeep** | Everything, including metadata, is represented as immutable content-addressed blobs. Storage needs only `put`, `get`, `stat/check` and digest-ordered enumeration. [web:91][web:92] | Whole-blob dedup; higher layers encode file schemas, claims and metadata as blobs. | Clean separation between dumb storage and indexing. Metadata cannot be overwritten, but search requires a separately maintained index. |
| **restic** | Snapshots and trees refer to encrypted, hash-identified content blobs. | Rabin-fingerprint CDC with a rolling window; boundaries depend on nearby content rather than absolute offsets. The documented design uses a 64-byte window and tests low fingerprint bits for a cut. [web:93][web:94] | Insertions normally disturb only chunks near the edit. Packing many chunks reduces object-store request overhead, but pack rewriting/compaction complicates deletion. |
| **Borg** | Repository-wide deduplication covers file data and metadata. | Borg 1.x uses Buzhash rolling-hash CDC and a cryptographic content ID for actual deduplication; default coarse chunks target roughly 2 MiB. Borg 2 introduces a faster FastCDC chunker. [web:409][web:411][web:412] | Smaller chunks improve dedup but increase indexes, memory, seeks and object counts; larger chunks reverse that trade-off. |
| **Kopia** | Snapshot manifests refer to immutable content objects in a repository. | Uses rolling-hash file splitting so a changed large file uploads only changed portions; documented analysis identifies Kopia’s default family as Buzhash with roughly 2-MiB average chunks, rather than FastCDC. [web:416][web:417] | Encrypted incremental snapshots can share chunks globally within the repository while policies govern snapshot retention. |
| **bup** | Stores backup chunks and tree structures using Git’s object and packfile machinery. | Splits streams using a rolling checksum, traditionally around 8 KiB average, checks whether each chunk already exists, and writes new chunks to Git packfiles. [web:107][web:112] | Adds CDC to Git’s whole-blob model, making large VM images and databases incrementally backup-friendly, at the cost of very many Git objects. |
| **casync** | An index lists the ordered digests needed to reconstruct a blob or archive; chunks live in a shared chunk store. | Variable-size CDC, traditionally Buzhash, followed by cryptographic hashing and per-chunk compression. The client downloads only absent chunks. [web:115][web:116] | Operates like “rsync with chunks prepublished in an HTTP-addressable store,” allowing arbitrary old local data to serve as a seed. |
| **desync** | Go reimplementation compatible with casync stores, `caibx`/`caidx` indexes and `catar` archives. | Same chunk boundaries as casync but parallelized; typical chunks are 16–256 KiB, addressed by SHA-512/256 and compressed with zstd. [web:169][web:176] | Better parallel creation while retaining casync wire/data compatibility. |
| **SquashFS image approach** | A release is commonly one compressed, read-only filesystem image, optionally addressed by its whole-image digest. | Files and metadata are packed into separately compressed fixed-size blocks, default 128 KiB and up to 1 MiB. [web:436] | Excellent compactness and mount-time random access, but image-layout changes can disturb many compressed blocks. Pair with block indexes, casync or binary deltas to avoid full downloads. |
| **OSTree** | Git-like CAS of individual files and directory/commit metadata. A deployment checks out an immutable tree. | Identical files are stored once. Checkouts normally hardlink repository objects into deployment trees; static deltas can combine object transfer with bsdiff- and rsync-style operations. [web:166][web:172] | Multiple complete root filesystems coexist cheaply. Hardlinked files must remain immutable; mutable state belongs outside the deployment. Summary metadata can itself grow linearly with refs and static deltas. [web:167] |
| **ZFS dedup** | Not a user-visible Merkle artifact graph; dedup operates synchronously on filesystem blocks through a dedup table. | Every eligible write hashes and looks up its block in the DDT; identical blocks share physical storage. | Transparent but expensive: every unique block needs a DDT entry and every write/free consults it. OpenZFS calls dedup its most resource-intensive feature and gives a planning floor of about 1.25 GiB RAM per TiB stored, workload-dependent. [web:136] |
| **Btrfs dedup** | Filesystem extents can be shared using reflinks. | Upstream Btrfs implements only **out-of-band** dedup: an application finds equal ranges and asks the filesystem to share them. [web:141] | No permanent in-line hash lookup on every write, but a scanner or content-aware producer must discover duplicates. |
| **Bazel CAS + Action Cache** | CAS maps a digest to file/tree/stdout/stderr bytes. The Action Cache maps a hash of an action and its declared inputs/environment to an `ActionResult` containing output digests. [web:151][web:155] | Whole-file/tree CAS dedup; the build graph supplies semantic reuse. | Separating AC from CAS is crucial: the same output blob may serve many actions, while an AC miss only means “execute again.” An AC hit whose CAS blobs were evicted is stale and must be handled as a recoverable cache failure. |

## CDC versus fixed chunks versus binary deltas

- **Fixed chunks:** cheapest to compute and index; an insertion near the start shifts every later boundary.
- **Content-defined chunks:** rolling Rabin, Buzhash or Gear/FastCDC fingerprints select boundaries based on local bytes, so boundaries resynchronize after an insertion.
- **FastCDC:** uses a fast Gear-style rolling hash and normalized chunk-size distribution to reduce CPU cost while retaining CDC behavior. It is an algorithm family, not a synonym for every rolling-hash chunker.
- **Pairwise binary delta:** compares one known old version to one new version and can produce the smallest transfer, but requires generating and retaining edges for relevant version pairs.
- **CAS chunks:** one index for the new version can reuse chunks from *any* locally available version, avoiding an all-pairs delta matrix.

---

# 4. Efficient delta transfer

| Technique | How it works | Best use | Main limitations |
|---|---|---|---|
| **rsync** | Receiver partitions its old file into fixed blocks and sends a weak rolling checksum plus strong checksum for each. Sender rolls a window over the new file at every byte offset, emits references to matching old blocks and literal bytes for misses. [web:182][web:191] | Synchronizing arbitrary old and new files when both endpoints can participate. | Interactive protocol/signature exchange; CPU and memory scale with signature count; transport output is not automatically a reusable CDN artifact. |
| **zsync** | Publishes a control file containing block checksums. The client scans its existing file using the rsync rolling checksum, determines missing target ranges, then fetches those ranges from an ordinary HTTP server. [web:184][web:187] | CDN/static-web distribution where clients may possess an older full file. | Target file must be laid out so HTTP byte ranges correspond to needed blocks; many sparse ranges can cause request amplification. |
| **bsdiff / bspatch** | Builds an index—classically a suffix array—over the old file, finds exact and approximate matches, and encodes control, byte-difference and extra streams, traditionally bzip2-compressed. [web:212][web:214] | Very compact pairwise patches, especially executables and moderately sized images. | Patch generation is relatively CPU/RAM intensive; applying requires exact expected source bytes and space for safe reconstruction. |
| **Courgette** | Disassembles an executable, replaces unstable addresses with symbolic/normalized representations, applies bsdiff in transformed space, then reverses the transform. Chromium documented a sample reduction from a 704,512-byte bsdiff patch to 78,848 bytes. [web:211] | Architecture-aware Chrome executable updates. | File-format/architecture specific and more complex than byte-level diffing. |
| **Zucchini** | Successor-style executable differencing that discovers moved regions and references, then adjusts references into relocated code/data rather than encoding each changed address. It is used in Chrome updates. [web:216][web:225] | Modern executable and archive-aware browser updates. | Complex parsers; unsupported or malformed inputs need raw fallback paths. |
| **xdelta3 / VCDIFF** | Emits standardized VCDIFF windows and instructions that copy from the source or target and add/run new bytes. xdelta3 can process large streams with bounded source/input windows and optional secondary compression. [web:421][web:422][web:425] | General-purpose, streamable binary patches and interoperable tooling. | Usually larger than highly specialized executable-aware deltas; exact base-version management is still required. |
| **casync / desync** | Publishes an index of CDC chunk digests. Client scans one or more local seeds and downloads only missing compressed chunks from an HTTP/object store. | OS images, archives and large filesystem images where clients may start from several different versions. | More objects and metadata than one monolithic file; chunk size affects request count, dedup and compression ratio. |
| **OSTree static deltas** | Precomputes a delta between two commits, packing many small objects and optionally using bsdiff/rsync-style instructions; can also make a self-contained offline delta. [web:166][web:178] | Atomic Linux system-tree deployments and offline updates. | A version graph with many supported starting releases creates many delta edges; missing edges fall back to object transfer. |
| **Android A/B OTA** | The payload generator identifies zero blocks, unchanged source blocks (`SOURCE_COPY`), new compressed blocks (`REPLACE*`) and changed files suitable for `SOURCE_BSDIFF` or `PUFFDIFF`; operations are sorted and merged. [web:196] | Safe system updates while Android continues running. | Requires precise source-build compatibility; full payload fallback and temporary storage remain necessary. |

## Android’s running-system model

On an A/B device, the updater reads the active slot but writes the unused slot. The system continues operating from the old slot, verifies the new image, then switches slots at reboot; failure can revert to the known-good slot. [web:197][web:205]

Virtual A/B replaces some of the physical duplication with snapshots and copy-on-write storage, including compressed snapshots in supported releases. The general safety principle remains: **never patch executable bytes in place while they may be running**.

## Recommended pattern for minimal, safe binary updates

1. **Canonicalize builds:** deterministic ordering, timestamps, compression and debug-symbol handling greatly improve delta quality.
2. **Publish a signed target manifest:** include target digest, size, required source digests, patch digest, algorithm, platform and minimum updater version.
3. **Choose transfer mode in order:**
   - exact-base specialized delta;
   - reusable CDC index plus missing chunks;
   - complete artifact fallback.
4. **Download into a staging area or inactive slot.**
5. **Authenticate before use:** verify manifest/signature and each chunk or patch digest.
6. **Apply using copy-on-write or a temporary file**, not in-place mutation.
7. **Verify the final whole-artifact digest**, fsync data and metadata, then atomically switch a symlink, mount, boot slot or release pointer.
8. **Retain rollback state** until health checks succeed.
9. **Never treat the weak rolling checksum as integrity protection.** It finds candidate matches; a cryptographic digest confirms content.
10. **Avoid long delta chains.** Prefer direct base→target patches or CAS chunks; otherwise one missing/corrupt intermediate release breaks the chain.

---

# 5. Serving builds at scale

## Recommended serving architecture

```text
                 mutable control plane
 CI/publisher ──> release/tag metadata ──> signed root manifest
       │
       └────────> immutable digest URLs ──> origin object storage
                                            │
                                            └── CDN / regional caches
                                                   │
                                      clients fetch manifests, ranges,
                                      chunks or complete blobs
```

### CDN fronting

Digest-addressed objects are ideal CDN content:

- URLs never need invalidation after publication.
- They can use long `Cache-Control: public, max-age=..., immutable`.
- A tag or “latest” document should have a short TTL and resolve to a digest.
- The origin should reject digest/content mismatches and avoid overwriting immutable keys.
- Signed URLs/cookies can control private distribution, but overly user-specific cache keys reduce sharing.
- Shielding and request coalescing prevent a new release from causing a thundering herd at the origin.

### Range requests

HTTP range requests let a client request one or more byte subranges instead of the complete representation. A successful response uses `206 Partial Content` and `Content-Range`. [web:243]

They are useful for:

- zsync-like recovery;
- seekable archives and indexes;
- lazy image filesystems;
- resumable downloads; and
- reading file tables before payload data.

But range support is not enough by itself:

- CDN behavior differs—some cache the full file, some cache aligned slices.
- Excessive tiny or random ranges increase HTTP requests, TLS/authorization work and latency.
- Compressed streams need restart points; one ordinary gzip stream cannot generally be decompressed from an arbitrary compressed-byte offset.
- Origins must provide stable `Content-Length`, validators and byte-identical representations. Cloudflare, for example, returns a full response rather than a range from cache if the origin omitted `Content-Length`. [web:254]

## Lazy and partial fetching

| System | Lazy-fetch mechanism | Benefit | Trade-off |
|---|---|---|---|
| **Bazel Build without the Bytes** | Remote actions keep outputs in CAS; Bazel downloads only the subset needed locally, using `--remote_download_minimal` or top-level modes. [web:152] | Large intermediate outputs remain close to remote workers; less client bandwidth and disk use. | Local paths may represent remote-only outputs. Eviction during a build can yield missing blobs, retries or re-execution; cache leases/TTL coordination become important. |
| **Git partial clone** | A promisor remote promises to supply omitted objects later; `--filter=blob:none` initially fetches commits and trees but defers blobs. [web:256][web:258] | Small initial clone and less local storage. | Commands that unexpectedly traverse blobs can trigger network waterfalls; offline operation is incomplete. |
| **VFS for Git** | Virtualizes the working tree so tools see all paths, but downloads objects only as needed and narrows Git operations to accessed files. [web:259] | Makes very large repositories usable with conventional filesystem tools. | Filesystem virtualization complexity, first-open latency and network dependency. |
| **EdenFS** | Presents a virtual checkout and lazily fetches file information/data only for repository portions actually used. [web:270] | Scales monorepo working copies and avoids materializing most files. | Requires a daemon and specialized filesystem/invalidation machinery; pathological broad scans defeat laziness. |
| **stargz / eStargz** | Stores gzip-compatible layers with seekable metadata/checkpoints; a remote snapshotter mounts the image and fetches necessary chunks on file access. Containers can start before the complete image is downloaded. [web:226][web:232] | Faster cold start when an application touches only a small portion of a large image. | First-access page/file faults become registry requests; random workloads may eventually fetch almost everything more slowly. Recompression can reduce normal layer sharing, and the runtime needs local caching, verification and a compatible snapshotter. |
| **SquashFS over ranged/chunked transport** | Mount/read compressed blocks by index, or reconstruct the image from CAS chunks. | Compact read-only release with bounded random-access decompression. | Fixed compressed-block layout can reduce cross-version reuse; remote mounting ties application latency to network availability. |

### Core trade-off

Lazy fetching exchanges **up-front deterministic transfer** for **runtime tail latency and dependency on cache availability**. It is most valuable when the working set is much smaller than the artifact. For dense workloads that eventually read nearly every byte, bulk sequential download usually provides better throughput and simpler failure handling.

A robust implementation therefore adds:

- prefetch profiles based on startup traces;
- bounded local caches;
- request coalescing and read-ahead;
- whole-file or whole-layer fallback;
- cryptographic verification before exposing bytes;
- leases for blobs needed by active jobs; and
- metrics for fetched fraction, request count, first-read latency and cache misses.

---

# 6. Garbage collection in content-addressed stores

## Reference counting versus mark-and-sweep

| Approach | Mechanism | Advantages | Failure modes / costs |
|---|---|---|---|
| **Reference counting** | Maintain an incoming-reference count per blob; increment when an edge/root is created and decrement when removed; reclaim at zero. | Immediate reclamation; no full-store trace; attractive where relationships are transactional and acyclic. | Every reference mutation becomes a durable coordinated write. Lost increments can delete live data; lost decrements leak. Cycles do not reach zero, and eventual consistency or cross-region races make counts difficult to trust. |
| **Mark-and-sweep** | Snapshot a root set, traverse all reachable manifests/trees, mark their blobs, then reclaim unmarked objects. | Immutable blobs require no mutation; handles cycles; liveness is derived from authoritative roots. | Requires enumerating objects and edges, may need large mark state, and must protect objects created or rooted concurrently with the scan. |
| **Age/LRU/TTL eviction** | Remove objects not recently accessed or older than a deadline, regardless of semantic reachability. | Bounded, operationally simple and appropriate for recomputable caches. | It is eviction, not correctness-preserving GC. It can leave dangling metadata and break active builds unless clients can retry or recompute. |
| **Hybrid** | Trace durable releases; lease active sessions; age-delete unreferenced uploads; LRU-evict reproducible cache entries. | Usually the practical production design. | Requires clearly separating authoritative artifacts from expendable cache data. |

Classical reference counting deallocates an object at zero references, while mark-and-sweep traces from roots and frees all unmarked records. [web:301][web:303]

## Nix GC roots

Nix treats store paths—not individual files—as GC units. Roots are store paths referenced through the GC-root hierarchy, traditionally symlinks under `/nix/var/nix/gcroots`; profiles, system generations and explicit build-result roots keep their transitive closures alive. [web:271][web:273]

`nix-store --gc` deletes paths not reachable through filesystem references from the root set. Nix exposes the live and dead sets for inspection, and refuses ordinary deletion of paths still needed by live paths. [web:272][web:275]

This is a clean mark-and-sweep model:

```text
profiles / system generations / explicit result roots
                    │
                    ▼
            live Nix store paths
                    │ references
                    ▼
               dependencies

unreachable store paths → collect
```

The practical source of “mysterious Nix bloat” is often forgotten roots or old profile generations, not failure of content addressing.

## Bazel cache eviction

Bazel’s local disk cache can automatically GC by maximum size or entry age while Bazel is idle. Current flags include `--experimental_disk_cache_gc_max_size`, `--experimental_disk_cache_gc_max_age`, and an idle-delay setting. [web:288]

Remote caches are harder:

```text
AC[action digest] → ActionResult
                     ├── output file digest → CAS
                     ├── output tree digest → CAS
                     ├── stdout digest → CAS
                     └── stderr digest → CAS
```

If CAS blobs are evicted while the AC entry survives, the system reports an action-cache “hit” whose outputs cannot be materialized. This exact AC/CAS inconsistency has caused Bazel failures; HTTP caches historically could remove CAS entries without understanding their AC references. [web:286][web:287]

Bazel mitigations include:

- retrying cache-eviction failures;
- returning dedicated exit code 39;
- informing Bazel of a fixed cache TTL;
- periodically extending leases under LRU policies; and
- action rewinding/re-execution when a missing output is reproducible. [web:152]

## Why distributed shared-cache GC is difficult

1. **No single root set:** clients, schedulers, workers and other regions may each hold references unknown to the storage node.
2. **In-flight publication:** blobs are normally uploaded before the manifest or AC entry that references them. A concurrent sweep can misclassify them as garbage.
3. **In-flight reads:** a client may resolve a manifest, pause, then request a child blob after GC removed it.
4. **Replica lag:** a reference may be visible in one region before its blob, or vice versa.
5. **AC-to-CAS multiplicity:** many action results can reference the same blob; deleting one action result proves nothing about blob liveness.
6. **Mutable tags and immutable manifests:** the root changes atomically, but old readers may still be walking the previous graph.
7. **Referrers invert discovery:** signatures and SBOMs point to a subject; policy must decide whether subject liveness retains attachments, whether attachments retain subjects, or neither.
8. **Enumeration cost:** billions of small blobs make full scans, marks and object-store deletions expensive.
9. **Tenant isolation:** global physical dedup conflicts with per-tenant quotas, deletion guarantees and cryptographic-erasure expectations.
10. **Access timestamps are unreliable:** updating them on every read creates metadata-write load; sampled or delayed updates can evict hot objects.

### Safe distributed-GC pattern

A defensible design combines:

- **immutable digest objects;**
- **transactional root publication;**
- **temporary upload leases;**
- **a grace period longer than maximum replication and job duration;**
- **epoch-based mark-and-sweep or tombstones rather than immediate deletion;**
- **atomic AC deletion with, or before, CAS reclamation;**
- **reader leases for active executions;**
- **quarantine before physical removal;**
- **recomputation fallback for caches;**
- **final digest verification on every fetch.**

For an authoritative release repository, false deletion is unacceptable, so tracing and conservative grace periods dominate. For a Bazel-style cache, false deletion is tolerable only if the action is reproducible and the client correctly converts a dangling cache hit into a miss and re-execution.

---

# Overall design recommendations

| Requirement | Recommended design |
|---|---|
| Normal Maven/npm/PyPI/Deb/RPM consumption | Keep a package-aware registry; do not expose only raw CAS blobs. |
| One service for arbitrary release assets | OCI 1.1 + ORAS, with carefully chosen media types and digest pinning. |
| Maximum cross-version dedup for large images | Deterministic serialization plus CDC chunks and a signed Merkle/index root. |
| Smallest patch for one known base version | bsdiff, Zucchini/Courgette or xdelta, depending on file type and resource limits. |
| Many possible client base versions | casync/desync-style CDC is preferable to generating an all-pairs patch graph. |
| Safe live OS/application updates | Stage into an inactive slot or immutable deployment, verify fully, then atomically switch. |
| Very large remote builds | Bazel CAS/AC, CDN/regional CAS replicas and Build without the Bytes, with leases and eviction recovery. |
| Fast container startup | eStargz or another remote-snapshot format, but measure fetched fraction and request amplification. |
| Durable artifact retention | Mark-and-sweep from signed release roots, with upload/read leases and a deletion grace period. |
| Recomputable build cache | Size/age/LRU eviction is acceptable, provided AC and CAS consistency or client recomputation is guaranteed. |

The central architectural lesson is that **content addressing solves identity, integrity and exact-byte deduplication—but not naming, lifecycle, authorization, metadata indexing, efficient sub-file reuse or distributed liveness**. Successful systems explicitly separate those responsibilities rather than expecting a single registry or hash table to solve all of them.

Citations:
[1] https://registry.terraform.io/providers/jfrog/artifactory/latest/docs/resources/package_cleanup_policy
[2] https://docs.jfrog.com/administration/docs/cleanup-policies
[3] https://crazymax.dev/artifactory-cleanup/config/policies/
[4] https://registry.terraform.io/providers/jfrog/artifactory/12.9.1/docs/resources/package_cleanup_policy
[5] https://www.pulumi.com/registry/packages/artifactory/api-docs/packagecleanuppolicy/
[6] https://help.sonatype.com/en/cleanup-policies-api.html
[7] https://jfrog.com/help/r/artifactory-rest-apis/get-all-package-cleanup-policies-api?contentId=p6M7WTr0iHoXKq4Z~uVFNQ
[8] https://nexus3-cli.readthedocs.io/_/downloads/en/3.0.4/pdf/
[9] https://jfrog.com/help/r/jfrog-rest-apis/get-all-cleanup-policies-api
[10] https://jfrog.com/help/r/artifactory-cleanup-policies-faq
[11] https://docs.jfrog.com/artifactory/docs/supported-package-types
[12] https://github.com/jfrog/artifactory-user-plugins
[13] https://help.sonatype.com/en/sonatype-nexus-repository-3-65-0-release-notes.html
[14] https://jfrog-guide.ford.com/ch03/04-retention-policies.html
[15] https://www.sonatype.com/blog/keeping-your-sonatype-nexus-repositories-clean-just-got-easier
[31] https://docs.aws.amazon.com/codeartifact/latest/ug/packages-overview.html
[32] https://docs.cloud.google.com/artifact-registry/docs/repositories/cleanup-policy
[33] https://docs.cloud.google.com/artifact-registry/docs/repositories/cleanup-policy-overview
[34] https://docs.cloud.google.com/artifact-registry/docs/supported-formats
[35] https://docs.aws.amazon.com/codeartifact/latest/ug/delete-package.html
[36] https://docs.aws.amazon.com/codeartifact/latest/APIReference/Welcome.html
[37] https://cloud.google.com/artifact-registry/docs/repositories/cleanup-policy-overview?hl=ja
[38] https://docs.aws.amazon.com/cli/latest/reference/codeartifact/
[39] https://docs.cloudsmith.com/formats
[40] https://cloudsmith.com/product/formats
[41] https://cloud.google.com/artifact-registry/docs/repositories/cleanup-policy-overview?hl=zh-cn
[42] https://docs.cloudsmith.com/supply-chain-security/policy-as-code/cooldown-policy
[43] https://cloud.google.com/artifact-registry/docs/repositories/cleanup-policy-overview?hl=zh-tw
[44] https://docs.cloudsmith.com/artifact-management/retention-rules
[45] https://cloud.google.com/artifact-registry/docs/repositories/cleanup-policy?hl=es-419
[16] https://github.com/opencontainers/image-spec/blob/main/spec.md
[17] https://oci-playground.github.io/specs-latest/specs/distribution/v1.1.0-rc2/oci-distribution-spec.html
[18] https://opencontainers.org/posts/blog/2024-03-13-image-and-distribution-1-1/
[19] https://oci-playground.github.io/specs-latest/specs/distribution/v1.0.1/oci-distribution-spec.pdf
[20] https://docs.rs/oci-spec/latest/oci_spec/image/index.html
[21] https://oras.land/docs/concepts/artifact
[22] https://lesitedefrancois.be/en/security/oci-referrers/
[23] https://help.aliyun.com/zh/acr/use-cases/operating-with-oci-image-and-distribution-specification-v1-1-0
[24] https://www.alibabacloud.com/help/en/acr/use-cases/operating-with-oci-image-and-distribution-specification-v1-1-0
[25] https://deepwiki.com/youki-dev/oci-spec-rs/4.2-image-specification
[26] https://oneuptime.com/blog/post/2025-12-08-oci-artifacts-explained/view
[27] https://aws.amazon.com/blogs/opensource/diving-into-oci-image-and-distribution-1-1-support-in-amazon-ecr/
[28] https://lesitedefrancois.be/en/security/oras/
[29] https://opencontainers.org/posts/blog/2023-07-07-summary-of-upcoming-changes-in-oci-image-and-distribution-specs-v-1-1/
[30] https://www.bretfisher.com/blog/oci-artifacts
[46] https://docs.github.com/packages/learn-github-packages/introduction-to-github-packages
[47] https://docs.github.com/en/enterprise-cloud@latest/packages/learn-github-packages/introduction-to-github-packages
[48] https://docs.gitlab.com/user/packages/container_registry/reduce_container_registry_storage/
[49] https://gitlab.com/gitlab-org/gitlab/-/merge_requests/226529
[50] https://docs.gitlab.com/development/packages/cleanup_policies/
[51] https://weblicht.sfs.uni-tuebingen.de/gitlab/help/user/packages/index.md
[52] https://docs.gitlab.com/administration/packages/container_registry/
[53] https://docs.github.com/en/enterprise-server@3.18/packages/learn-github-packages/introduction-to-github-packages
[54] https://docs.github.com/en/rest/packages/packages
[55] https://medium.com/@cedricdue/clearing-gitlabs-package-registry-automatically-from-the-ci-4fab353340c
[56] https://lorca.act.uji.es/gitlab/help/user/packages/container_registry/reduce_container_registry_storage.md
[57] https://stackoverflow.com/questions/73814623/gitlab-registry-cleanup-policy-how-to-keep-most-recent-n-tags-but-delete-other
[58] https://docs.gitlab.com/development/packages/
[59] https://runbooks.gitlab.com/package-registry/
[60] https://genboree.org/gitlab/help/user/packages/package_registry/supported_package_managers.md
[61] https://jfrog.com/artifactory/
[62] https://jfrog.com/help/r/jfrog-artifactory-documentation/repository-support-for-package-clients
[63] https://docs.jfrog.com/artifactory/docs/supported-package-types
[64] https://www.devopsschool.com/blog/package-types-supported-by-artifactory/
[65] https://help.sonatype.com/en/sonatype-nexus-repository.html
[66] https://jfrog.com/help/r/jfrog-platform-administration-documentation/supported-package-types-for-package-retention-policies?contentId=rhNz4m6PAsPp3vLI8n8Oyw
[67] https://www.w3architect.com/en/wiki/technologies/containerization/sonatype-nexus/
[68] https://help.sonatype.com/en/nexus-repository-feature-matrix.html
[69] https://gaia.esac.esa.int/nexus/service/outreach/?version
[70] https://docs.jfrog.com/installation/docs/direct-cloud-storage-download
[71] https://docs.jfrog.com/artifactory/docs/package-manager-compatibility-matrix
[72] https://www.sonatype.com/products/sonatype-nexus-repository
[73] https://docs.jfrog.com/artifactory/docs/federated-repository-supported-package-types
[74] https://docs.jfrog.com/artifactory/docs/jfrog-artifactory
[75] https://help.sonatype.com/en/repository-types.html
[76] https://www.questjournals.org/jses/papers/Vol9-issue-12/09128094.pdf
[77] https://support.sonatype.com/hc/en-us/articles/213465108-How-to-reduce-the-disk-space-used-by-System-Feeds-and-increase-performance
[78] https://community.sonatype.com/t/s3-storage-increased-10x-over-8-months/12171
[79] https://github.com/sonatype/nexus-public/issues/613
[80] https://atlantbh.com/blog/nexus-upgrade-adventures/
[81] https://github.com/sonatype/nexus-public/issues/468
[82] https://docs.github.com/en/billing/concepts/product-billing/github-packages
[83] https://www.linkedin.com/posts/mayurpanchale33_devops-nexusrepository-docker-activity-7431269040171442176-d1fR
[84] https://stackoverflow.com/questions/76819188/after-deleting-the-docker-images-from-nexus-repository-web-interface-the-space-i
[85] https://medium.com/@dogukancagatay/docker-repository-cleanup-for-sonatype-nexus-c46269b9969d
[86] https://grundlefleck.github.io/2019/11/05/working-around-slow-deletes-of-components-in-nexus3-oss.html
[87] https://docs.gitlab.com/user/packages/package_registry/
[88] https://stackoverflow.com/questions/71055881/nexus-repository-manager-stuck-in-storage-facet-cleanup-loop
[89] https://www.reddit.com/r/devops/comments/yw1pw3/gitlab_paid_storage_update_psa/
[90] https://sonatype1699040277.zendesk.com/hc/en-us/articles/36507312816915-How-can-I-optimize-Nexus-Repository-2-to-minimize-disk-space-used
[121] https://git-scm.com/docs/pack-format
[122] https://github.blog/open-source/git/gits-database-internals-i-packed-object-store/
[123] https://stackoverflow.com/questions/5176225/are-gits-pack-files-deltas-rather-than-snapshots
[124] https://www.kernel.org/pub/software/scm/git/docs/git-pack-objects.html
[125] https://www.npmjs.com/package/ipfs-unixfs-importer/v/9.0.7-rc.6
[126] https://github.com/ipfs/specs/blob/main/UNIXFS.md
[127] https://gitperf.com/chapter-06.html
[128] https://lib.rs/crates/ferripfs-unixfs
[129] https://docs.rs/helia-unixfs/latest/helia_unixfs/
[130] https://proto.school/merkle-dags/04/
[131] https://git-scm.com/book/en/v2/Git-Internals-Packfiles
[132] https://indico.cern.ch/event/1075584/contributions/4659088/attachments/2377497/4061608/Intro%20to%20IPFS%20-%20CERN%20CS3.pdf
[133] https://gist.github.com/matthewmccullough/2695758
[134] https://deepwiki.com/ipfs/kubo/7.1-unixfs-and-content-addressing
[135] https://deepwiki.com/ipfs/kubo/4.1-add-and-data-ingestion
[91] https://perkeep.org/doc/overview.md
[92] https://github.com/perkeep/perkeep/blob/master/doc/overview.txt
[93] https://restic.net/blog/2015-09-12/restic-foundation1-cdc/
[94] https://github.com/restic/chunker
[95] https://perkeep.org/doc/
[96] https://pkg.go.dev/github.com/restic/chunker
[97] https://csyhua.github.io/csyhua/hua-atc2016.pdf
[98] https://eprint.iacr.org/2025/558.pdf
[99] https://slides.com/thomaswaldmann/borgbackup-deeper-faq
[100] https://pkg.go.dev/github.com/glycerine/restic-chunker-mod
[101] https://perkeep.org/pkg/blobserver
[102] https://docs.rs/fastcdc/latest/src/fastcdc/lib.rs.html
[103] https://lobste.rs/s/h38buk/restic_0_14_0_deduplicating_encrypted
[104] https://forum.restic.net/t/restic-flow-chart/7252/2
[105] https://www.scribd.com/document/1008977715/FastCDC-1
[106] https://blogs.kde.org/2011/06/02/bup-backup-tool-clever-idea/
[107] https://github.com/bup/bup
[108] https://ostreedev.github.io/ostree/reference/ostree-OstreeRepo.html
[109] https://github.com/williamstein/bup-1
[110] https://github.com/bup/bup/blob/main/README.md
[111] https://github.com/Zearin/git-bup
[112] https://man.cx/bup-split(1)
[113] https://apenwarr.ca/diary/2011-bup-git.pdf
[114] https://manpages.ubuntu.com/manpages/resolute//man1/casync.1.html
[115] https://deepwiki.com/systemd/casync
[116] https://archive.fosdem.org/2018/schedule/event/containers_casync/attachments/slides/2558/export/events/attachments/containers_casync/slides/2558/Exploring_container_image_distribution_with_casync_(fosdem).pdf
[117] https://lwn.net/Articles/380983/
[118] https://lazka.github.io/pgi-docs/OSTree-1.0/index.html
[119] https://man.archlinux.org/man/gitformat-pack.5.en.raw
[120] https://man.archlinux.org/man/ostree-checkout.1.en
[166] https://ostreedev.github.io/ostree/formats/
[167] https://ostreedev.github.io/ostree/repo/
[168] https://manpages.opensuse.org/Leap-15.6/libostree/ostree-checkout.1.en.html
[169] https://github.com/folbricht/desync/blob/master/README.md
[170] https://man.archlinux.org/man/ostree-static-delta.1.en
[171] https://manpages.ubuntu.com/manpages/bionic/en/man1/ostree-pull.1.html
[172] https://ostreedev.github.io/ostree/
[173] https://docs.foundries.io/97/reference-manual/ota/static-deltas.html
[174] https://github.com/folbricht/desync
[175] https://mail.gnome.org/archives/ostree-list/2013-July/msg00005.html
[176] https://pkg.go.dev/github.com/folbricht/desync
[177] https://www.foundries.io/insights/blog/ostree-static-deltas/
[178] https://ostreedev.github.io/ostree/copying-deltas/
[179] https://github.com/ostreedev/ostree/blob/main/src/libostree/ostree-repo-static-delta-compilation.c
[180] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/8/html/composing_installing_and_managing_rhel_for_edge_images/creating-and-managing-ostree-image-updates_composing-installing-managing-rhel-for-edge-images
[136] https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Data%20Storage/Deduplication.html
[137] https://openzfs.org/w/images/8/8d/ZFS_dedup.pdf
[138] https://github.com/openzfs/openzfs-docs/blob/master/docs/Performance%20and%20Tuning/Workload%20Tuning.rst
[139] https://www.truenas.com/docs/references/zfsdeduplication/
[140] https://openzfs.github.io/openzfs-docs/man/v2.3/4/zfs.4.html
[141] https://btrfs.readthedocs.io/en/latest/Deduplication.html
[142] https://openzfs.github.io/openzfs-docs/man/master/4/zfs.4.html
[143] https://klarasystems.com/articles/introducing-openzfs-fast-dedup/
[144] https://wiki.niziak.spox.org/linux:fs:zfs:dedup
[145] https://zfsstoragehub.com/posts/zfs-hardware-requirements-ram-hbas-and-disks/
[146] https://docs.oracle.com/cd/E36784_01/html/E39134/fsdedup-1.html
[147] https://openzfs.github.io/openzfs-docs/Project%20and%20Community/FAQ.html
[148] https://despairlabs.com/blog/posts/2024-10-27-openzfs-dedup-is-good-dont-use-it/
[149] https://github.com/openzfs/zfs/issues/6116
[150] https://github.com/openzfs/zfs/issues/10552
[151] https://bazel.build/remote/caching
[152] https://blog.bazel.build/2023/10/06/bwob-in-bazel-7.html
[153] https://www.incredibuild.com/blog/bazel-caching-remote-execution-and-the-build-supply-chain
[154] https://bazel.build/versions/8.6.0/remote/caching
[155] https://bazel.googlesource.com/bazel/+/refs/heads/staging/src/main/java/com/google/devtools/build/lib/remote/README.md
[156] https://github.com/buchgr/bazel-remote
[157] https://github.com/bazelbuild/bazel/issues/8250
[158] https://bazel.build/versions/6.3.0/remote/caching
[159] https://christianfscott.com/bazel-readonly-action-cache/
[160] https://github.com/bazelbuild/bazel/commit/24b45890c431de98d586fdfe5777031612049135
[161] https://blog.engflow.com/2024/05/13/the-many-caches-of-bazel/
[162] https://blog.bazel.build/2019/05/07/builds-without-bytes.html
[163] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[164] https://deepwiki.com/bazelbuild/bazel/6.1-remote-execution-and-caching
[165] https://blog.engflow.com/2023/12/11/bazel-7-is-here---and-bazel-invocation-analyzer-is-ready-for-it/
[181] https://www.samba.org/rsync/tech_report/node3.html
[182] https://www.jfranken.de/homepages/johannes/vortraege/mirroring/tech_report.pdf
[183] https://code.activestate.com/recipes/577518-rsync-algorithm/download/1/
[184] https://zsync.moria.org.uk/paper/ch02s03.html
[185] https://www.cnblogs.com/jiangzhaowei/p/8185572.html
[186] https://www.cnblogs.com/f-ck-need-u/p/7220753.html
[187] https://zsync.moria.org.uk/paper200503/ch02.html
[188] https://zsync.moria.org.uk/paper200501/ch02.html
[189] https://docs.openstack.org/freezer/queens/reference/api/freezer.engine.rsync.pyrsync.html
[190] https://github.com/librsync/librsync/blob/master/doc/rdiff.md
[191] https://rsync.samba.org/tech_report/node2.html
[192] https://support.i-s-e.com/downloads/ISE%20MS%20Windows%20Software%20Library/Schedule%20V7.7.18%20Distribution%20-%20ClientServer%20-%20May%205,%202017/html/schwindows/3211.htm
[193] https://github.com/dropbox/fast_rsync
[194] https://www.geeksforgeeks.org/system-design/rsync-algorithm-system-design/
[195] https://deepwiki.com/RsyncProject/rsync/2.2-delta-transfer
[211] https://www.chromium.org/developers/design-documents/software-updates-courgette/
[212] https://suntong30.github.io/assets/pdf/TMC24_sdiff.pdf
[213] https://neugierig.org/software/chromium/notes/2009/05/courgette.html
[214] https://www.daemonology.net/papers/bsdiff.pdf
[215] https://directory.fsf.org/wiki/Bsdiff
[216] https://bugzilla.mozilla.org/show_bug.cgi?id=1632374
[217] https://datatracker.ietf.org/doc/html/draft-korn-vcdiff-01
[218] https://github.com/blankbro/bsdiff-4.2
[219] https://www.chromium.org/chromium-os/chromiumos-design-docs/autoupdate-details/
[220] https://www.mankier.com/package/bsdiff
[221] https://github.com/chromium/chromium/blob/main/components/zucchini/zucchini_gen.h
[222] https://chromium.googlesource.com/chromium/src/+/2dc643d0f9..1b223e58f3
[223] https://www.diva-portal.org/smash/get/diva2:1538229/FULLTEXT01.pdf
[224] https://hydraulic.dev/blog/20-deltas-diffed.html
[225] https://chromium.googlesource.com/chromium/src/+/HEAD/components/zucchini/
[196] https://android.googlesource.com/platform/system/update_engine/+/refs/heads/master
[197] https://source.android.com/docs/core/ota/ab
[198] https://source.android.com/docs/core/ota/virtual_ab
[199] https://source.android.com/docs/core/ota/virtual_ab/implement
[200] https://android.googlesource.com/platform/system/update_engine/
[201] https://android.googlesource.com/platform/system/update_engine/+/37b9b709252f6d2f0f0a8c4e3f453fad357a17a8
[202] https://android.googlesource.com/platform/system/update_engine/+/refs/heads/android-s-beta-4
[203] https://android.googlesource.com/platform/system/update_engine.git/+show/387cd624b39cd7eaa911d2df820c37f4cc8e697c/README.md
[204] https://lpc.events/event/11/contributions/1049/attachments/826/1562/2021%20LPC_%20dm-snapshot%20in%20user%20space.pdf
[205] https://source.android.com/docs/core/ota
[206] https://docs.digi.com/resources/documentation/digidocs/embedded/android/dea11/cc8x/android_t_update-android-with-ota-package
[207] https://docs.digi.com/resources/documentation/digidocs/embedded/android/dea11/cc8mmini/android_t_update-android-with-ota-package
[208] https://source.android.com/docs/core/ota/tools
[209] https://www.esper.io/blog/firmware-over-the-air-fota-updates-on-android
[210] https://source.android.google.cn/docs/core/ota?hl=ja
[241] https://blog.blazingcdn.com/en-us/http-range-requests-how-video-seeking-works-over-a-cdn
[242] https://docs.cloud.google.com/media-cdn/docs/caching
[243] https://datatracker.ietf.org/doc/html/rfc9110
[244] https://docs.cloud.google.com/cdn/docs/caching
[245] https://learn.microsoft.com/lv-lv/Azure/cdn/cdn-large-file-optimization
[246] https://rfcinfo.com/rfc-9110/14/
[247] https://gcore.com/docs/cdn/cdn-resource-options/optimize-large-file-delivery
[248] https://support.huaweicloud.com/eu/usermanual-cdn/cdn_01_0027.html
[249] https://support.huaweicloud.com/intl/en-us/usermanual-cdn/cdn_01_0027.html
[250] https://www.rfc-editor.org/rfc/rfc9110.html
[251] https://sparkles-docs.pages.dev/research/autological-artifacts/range-request-access
[252] https://doc.cdnvideo.ru/en/Services_setup/http/
[253] https://www.restguide.info/rfc-9110
[254] https://developers.cloudflare.com/cache/concepts/default-cache-behavior/
[255] https://kevincox.ca/2021/06/04/http-range-caching/
[256] https://github.com/git/git/blob/master/Documentation/technical/partial-clone.txt
[257] https://gitperf.com/chapter-11.html
[258] https://www.kernel.org/pub/software/scm/git/docs/technical/partial-clone.html
[259] https://github.com/microsoft/vfsforgit
[260] https://stackoverflow.com/questions/68811902/can-you-convert-an-existing-git-repository-to-be-a-blobless-one
[261] https://github.com/microsoft/VFSForGit
[262] https://deepwiki.com/microsoft/VFSForGit
[263] https://git.js.cn/docs/partial-clone
[264] https://qiita.com/irgaly/items/0aace5cbd44aa4220733
[265] https://github.com/git/git/commit/faf2abf496bb8e5a5fbf3818f3e78077b2f3e143
[266] https://help.aliyun.com/en/yunxiao/user-guide/introduction-to-partial-clone
[267] http://github.com/Microsoft/GVFS
[268] https://microsoft.github.io/code-with-engineering-playbook/source-control/git-guidance/git-lfs-and-vfs/
[269] https://deepwiki.com/korfuri/awesome-monorepo/3.2-file-system-virtualization
[270] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Overview.md
[226] https://github.com/containerd/stargz-snapshotter
[227] https://github.com/containerd/stargz-snapshotter/blob/main/docs/estargz.md
[228] https://archive.fosdem.org/2021/schedule/event/containers_lazy_pull/attachments/slides/4687/export/events/attachments/containers_lazy_pull/slides/4687/stargz_snapshotter.pdf
[229] https://github.com/containerd/stargz-snapshotter/blob/main/docs/INSTALL.md
[230] https://github.com/containerd/stargz-snapshotter/blob/main/README.md
[231] https://crazymax.dev/buildkit/user-guides/lazy-pulling/
[232] https://github.com/containerd/stargz-snapshotter/blob/main/docs/overview.md
[233] https://github.com/containerd/nerdctl/blob/main/docs/stargz.md
[234] https://deepwiki.com/containerd/stargz-snapshotter/1.1-architecture
[235] https://deepwiki.com/containerd/stargz-snapshotter/1.2-key-concepts
[236] https://oneuptime.com/blog/post/2026-02-09-lazy-pulling-stargz-snapshotter/view
[237] https://containerd.io/docs/2.1/remote-snapshotter/
[238] https://av.tib.eu/media/52447
[239] https://es.slideshare.net/slideshow/estargzlazy-pulling/251120328
[240] https://blog.zmalik.dev/p/lazy-pulling-container-images-a-deep
[301] http://infolab.stanford.edu/~ullman/dragon/w06/lectures/gc.pdf
[302] https://stackoverflow.com/questions/50340329/do-dead-branches-in-git-live-forever-in-the-history-of-the-repo
[303] https://www.cs.cmu.edu/~janh/courses/411/23/lec/23-gc.pdf
[304] https://animeshgaitonde.medium.com/garbage-collection-algorithm-mark-sweep-ed874272702d
[305] https://concurrency.ch/Content/lectures/Compiler/Lecture08.pdf
[306] https://shbhmrzd.github.io/systems/garbage-collection/memory-management/2026/04/01/garbage-collectors-deep-dive.html
[307] https://github.com/kolemare/GarbageCollectors
[308] https://homes.cs.washington.edu/~bodik/ucb/cs164/sp12/lectures/22-garbage%20collection-sp12.pdf
[309] https://www.cs.cmu.edu/~fp/courses/15411-f14/lectures/21-gc.pdf
[310] https://www.cs.mcgill.ca/~cs520/2021/slides/20-gc.pdf
[311] https://git-memo.readthedocs.io/en/latest/garbage_collecting.html
[312] https://users.cs.northwestern.edu/~stamourv/teaching/321-S19/13-14-gc-mark-and-sweep.pdf
[313] https://www.cs.cmu.edu/afs/cs/academic/class/15745-s06/web/handouts/garbage.pdf
[314] https://www.cs.cmu.edu/~janh/courses/411/24/lectures/22-gc.pdf
[315] https://www.cs.umd.edu/class/fall2020/cmsc420-0301/files/memmanage.pdf
[271] https://releases.nixos.org/nix/nix-2.17.2/manual/package-management/garbage-collector-roots.html
[272] https://man.archlinux.org/man/nix-store-delete.1.en
[273] https://nixos.org/guides/nix-pills/11-garbage-collector.html
[274] https://nix.dev/manual/nix/2.24/command-ref/nix-store/gc
[275] https://nix.dev/manual/nix/2.26/command-ref/nix-store/gc
[276] https://nix.dev/manual/nix/2.22/command-ref/nix-store/gc
[277] https://nix.dev/manual/nix/2.28/command-ref/nix-store/gc
[278] https://nix.dev/manual/nix/2.31/command-ref/nix-store/gc
[279] https://nix.dev/manual/nix/2.34/command-ref/nix-store/gc.html
[280] https://www.mankier.com/1/nix-store
[281] https://nix.dev/manual/nix/2.26/package-management/garbage-collection
[282] https://ianthehenry.com/posts/how-to-learn-nix/saving-your-shell/
[283] https://wiki.nixos.org/wiki/Garbage_Collection
[284] https://dominicegginton.dev/documents/nix-gc-roots/
[285] https://www.reddit.com/r/NixOS/comments/1r4et5s/didnt_learn_about_nix_garbage_collector_until/
[286] https://github.com/bazelbuild/bazel/issues/21777
[287] https://bazel.googlesource.com/bazel/+/5d815790a0f5007068ed87a757e0f20aa80020ad%5E!/
[288] https://bazel.build/remote/caching
[289] https://bitrise.io/blog/post/bazel-remote-caching-api
[290] https://github.com/bazelbuild/bazel/pull/17538
[291] https://github.com/bazelbuild/bazel/issues/3452
[292] https://www.incredibuild.com/blog/bazel-caching-remote-execution-and-the-build-supply-chain
[293] https://github.com/bazelbuild/bazel/commit/aafe1235c55f6cdcfc577a40736aaeb9ebaca23b
[294] https://buildfetch.com/docs/buildfetch-cache/bazel-remote-caching/
[295] https://www.hermetiq.com/learn/errors/buildbarn-cas-not-found
[296] https://groups.google.com/g/bazel-dev/c/WwNN4kiYSpc
[297] https://bazel.build/remote/cache-remote
[298] https://bazel.googlesource.com/bazel/+/refs/heads/staging/src/main/java/com/google/devtools/build/lib/remote/README.md
[299] https://github.com/sluongng/bazel/commit/5406c953574d48434d33b1cf49b70b718795d783
[300] https://github.com/fweikert/bazel/commit/aafe1235c55f6cdcfc577a40736aaeb9ebaca23b
[316] https://aws.amazon.com/codeartifact/pricing/
[317] https://www.cloudrepo.io/articles/cloudsmith-pricing-guide
[318] https://cloud.google.com/artifact-registry/pricing
[319] https://www.gappsy.com/tools/cloudsmith/
[320] https://cloudpricecheck.com/gcp/artifact-registry-pricing
[321] https://cloudburn.io/tools/aws-codeartifact-pricing-calculator
[322] https://www.factualminds.com/tools/aws-codeartifact-pricing-calculator/
[323] https://tech-insider.org/ecr-vs-acr-vs-artifact-registry-2026/
[324] https://awsglossary.org/terms/codeartifact
[325] https://aws.amazon.com/codeartifact/
[326] https://aws-periodic-table.team-skynet.io/services/car
[327] https://cloudguard.dev/blog/artifact-registry-cleanup
[328] https://medium.com/@alina.glumova/choosing-between-codeartifact-and-nexus-our-decision-making-process-c9679b606b92
[329] https://cloudsmith.com/pricing
[330] https://blogs.businesscompassllc.com/2026/04/mastering-aws-codeartifact-publish-your.html
[346] https://docs.jfrog.com/installation/docs/backups
[347] https://handbook.gitlab.com/handbook/engineering/data-engineering/database-excellence/database-frameworks/doc/container-registry/
[348] https://jfrog.com/whitepaper/best-practices-for-managing-your-artifactory-filestore-2/
[349] https://media.jfrog.com/wp-content/uploads/2018/12/17144304/Best_Practices_Artifactory_Backups_and_DR_v2-1.pdf
[350] https://jfrog.com/help/r/comprehensive-guide-to-migrating-jfrog-artifactory/artifactory-checksum-based-storage
[351] https://docs.jfrog.com/installation/docs/checksum-based-storage-implementation
[352] https://docs.gitlab.com/user/packages/container_registry/reduce_container_registry_storage/
[353] https://docs.gitlab.com/administration/packages/container_registry/
[354] https://docs.gitlab.co.jp/ee/architecture/blueprints/container_registry_metadata_database/
[355] https://docs.gitlab.com/administration/packages/container_registry_metadata_database/
[356] https://jfrog.com/reference-architecture/self-managed/deployment/considerations/storage/
[357] https://handbook.gitlab.com/handbook/engineering/architecture/design-documents/cells/impacted_features/container-registry/
[358] https://gitlab.com/groups/gitlab-org/-/work_items/21037
[359] https://docs.jfrog.com/installation/docs/checksum-based-storage
[360] https://about.gitlab.com/blog/next-generation-container-registry/
[331] https://aws.amazon.com/codeartifact/pricing/
[332] https://cloudguard.dev/blog/artifact-registry-cleanup
[333] https://www.cloudrepo.io/articles/cloudsmith-pricing-guide
[334] https://future-architect.github.io/articles/20260519a/
[335] https://noh.ink/en/translated-articles/34Nw4sBL8TJ0F4yDeBZZ
[336] https://dev.to/kheai/zeroing-out-the-bill-the-developers-practical-guide-to-eliminating-google-cloud-gcp-idle-costs-34c3
[337] https://aws.amazon.com/codeartifact/sla/
[338] https://github.com/firebase/firebase-tools/issues/5935
[339] https://noh.ink/articles/34Nw4sBL8TJ0F4yDeBZZ
[340] https://cloud.google.com/artifact-registry/pricing
[341] https://techresolve.blog/2026/03/09/im-not-selling-anything-fix-your-gcr-gar-bucket/
[342] https://support.cloudsmith.com/hc/en-us
[343] https://cloudburn.io/tools/aws-codeartifact-pricing-calculator
[344] https://stackoverflow.com/questions/64556198/gcp-how-to-prune-maintain-artifact-registry-storage
[345] https://www.g2.com/products/cloudsmith/reviews?qs=pros-and-cons
[361] https://github.com/marketplace/actions/container-retention-policy
[362] https://github.com/actions/delete-package-versions
[363] https://github.com/marketplace/actions/delete-package-versions
[364] https://deepwiki.com/actions/delete-package-versions
[365] https://github.com/orgs/community/discussions/105099
[366] https://app.dosu.dev/e3630b91-3a35-46b9-a8d3-b0c1b3ef6331/documents/6c94cac0-af37-41f6-8a8b-ea6bf731b370
[367] https://gitlab.com/gitlab-org/gitlab/-/merge_requests/89055
[368] https://github.com/orgs/community/discussions/38962
[369] https://docs.gitlab.com/development/packages/cleanup_policies/
[370] https://github.com/aws-samples/codeartifact-retention-policy/blob/main/README.md
[371] https://github.com/marketplace/actions/delete-old-packages
[372] https://deepwiki.com/dataaxiom/ghcr-cleanup-action/1-overview
[373] https://forum.gitlab.com/t/keep-timeout-for-package-registry/90001
[374] https://github.com/snok/container-retention-policy/blob/main/README.md
[375] https://gitlab.com/groups/gitlab-org/-/epics/3184
[391] https://registry.terraform.io/providers/jfrog/artifactory/latest/docs/resources/package_cleanup_policy
[392] https://help.sonatype.com/en/cleanup-policies.html
[393] https://pkg.go.dev/github.com/williamt1997/go-nexus-client/nexus3/schema/cleanuppolicies
[394] https://atlantbh.com/blog/nexus-upgrade-adventures/
[395] https://docs.jfrog.com/administration/docs/cleanup-policies
[396] https://docs.jfrog.com/administration/docs/archive
[397] https://nexus3-cli.readthedocs.io/_/downloads/en/3.0.4/pdf/
[398] https://docs.cloudsmith.com/artifact-management/retention-rules
[399] https://github.com/jfrog/artifactory-user-plugins/blob/master/cleanup/cleanDockerImages/README.md
[400] https://help.sonatype.com/en/sonatype-nexus-repository-3-65-0-release-notes.html
[401] https://help.cloudsmith.io/docs/retention-rules
[402] https://stackoverflow.com/questions/64029890/artifactory-old-artifacts-clean-up-aql-with-retain-latest-3-versions
[403] https://nexus3-cli.readthedocs.io/en/2.3.1/nexuscli.cli.html
[404] https://galaxy.ansible.com/ui/repo/published/cloudkrafter/nexus/content/role/nexus_oss/
[405] https://github.com/tallandtree/nxrm3-cleaning
[376] https://docs.cloud.google.com/artifact-registry/docs/supported-formats
[377] https://cloud.google.com/artifact-registry/docs/supported-formats?hl=de
[378] https://cloud.google.com/artifact-registry/docs/supported-formats?hl=fr
[379] https://cloudsmith.com/product/formats/docker-registry
[380] https://cloud.google.com/artifact-registry/docs/supported-formats?hl=ja
[381] https://cloud.google.com/artifact-registry/docs/supported-formats?hl=zh-tw
[382] https://cloud.google.com/artifact-registry/docs/supported-formats?hl=ko
[383] https://cloudsmith.com/product/formats/raw-repository
[384] https://cloudsmith.com/product/formats
[385] https://nicheelab.com/en/articles/gcp/artifact-registry-guide/
[386] https://cloudsmith.com/product/formats/debian-repository
[387] https://docs.cloudsmith.com/formats/debian-repository
[388] https://cloud.google.com/dotnet/docs/reference/Google.Cloud.ArtifactRegistry.V1/latest/Google.Cloud.ArtifactRegistry.V1.Repository.Types.Format
[389] https://docs.cloud.google.com/artifact-registry/docs/release-notes
[390] https://docs.cloud.google.com/artifact-registry/docs/repositories/remote-overview
[406] https://github.com/borgbackup/borg/discussions/6639
[407] https://borgbackup.readthedocs.io/_/downloads/en/1.0.0/pdf/
[408] https://borgbackup.readthedocs.io/en/1.0.11/internals.html
[409] https://www.borgbackup.org/releases/borg-2.0.html
[410] https://borgbackup.readthedocs.io/en/2.0.0b17/usage/notes.html
[411] https://borgbackup.readthedocs.io/en/1.2.6/internals/data-structures.html
[412] https://borgbackup.readthedocs.io/en/1.2.8/usage/notes.html
[413] https://borgbackup.readthedocs.io/en/1.0-maint/internals.html
[414] https://kopia.discourse.group/t/does-kopia-use-content-defined-chunking-cdc/1417
[415] https://borgbackup.readthedocs.io/en/2.0.0b7/usage/notes.html
[416] https://www.research-collection.ethz.ch/server/api/core/bitstreams/fcf57188-aeed-41d6-bddf-58cdfeb0502f/content
[417] https://kopia.io/docs/features/
[418] https://borgbackup.readthedocs.io/en/stable/internals.html
[419] https://github.com/tigerwill90/fastcdc
[420] https://kopia.io/docs/reference/command-line/common/benchmark-splitter/
[436] https://docs.kernel.org/filesystems/squashfs.html
[437] https://docs.kernel.org/6.2/filesystems/squashfs.html
[438] https://docs.kernel.org/6.0/filesystems/squashfs.html
[439] https://docs.kernel.org/5.17/filesystems/squashfs.html
[440] https://www.tldp.org/HOWTO/html_single/SquashFS-HOWTO/
[441] https://blog.csdn.net/gqb_driver/article/details/12946629
[442] https://en.wikipedia.org/wiki/SquashFS
[443] https://www.admin-magazine.com/HPC/Articles/Read-only-File-Compression-with-SquashFS
[444] https://github.com/systemd/casync/issues/46
[445] https://dr-emann.github.io/squashfs/squashfs.html
[446] http://mirror.internode.on.net/pub/ldp/en/SquashFS-HOWTO/SquashFS-HOWTO.txt
[447] https://dr-emann.github.io/squashfs/
[448] https://mgorny.pl/articles/using-deltas-to-speed-up-squashfs-ebuild-repository-updates.pdf
[449] http://muko.damember.org/rauc/changes.html
[450] https://github.com/advancedtelematic/aktualizr/blob/master/docs/ota-client-guide/modules/ROOT/pages/comparing-full-filesystem-update-strategies.adoc
[421] https://github.com/jmacd/xdelta
[422] https://www.mankier.com/1/xdelta3
[423] https://jmacd.github.io/xdelta/
[424] https://android.googlesource.com/platform/external/xdelta3/+/990c1ebf2f02d2ce5086983002ca466012088f6d/xdelta3.c
[425] https://android.googlesource.com/platform/external/xdelta3/+/4c83feb042c16cb12dcb666c15ec7e8017fdd8ac/xdelta3.c
[426] https://www.mankier.com/package/xdelta
[427] https://github.com/ChronoMonochrome/xdelta3
[428] https://github.com/jmacd/xdelta/issues/193
[429] https://en.wikipedia.org/wiki/Xdelta
[430] https://datatracker.ietf.org/doc/html/rfc3284
[431] https://github.com/marcprux/swift-xdelta/
[432] https://en.wikipedia.org/wiki/VCDIFF
[433] https://android.googlesource.com/platform/external/xdelta3/+/20e70e0ea30c1c13e313f4f0a68b7db031d01890/README
[434] https://github.com/hoytech/Vcdiff
[435] https://www.reddit.com/r/compression/comments/u1ajt7/binary_delta/
