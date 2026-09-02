<!-- Raw research source. Generated via Perplexity `perplexity_reason` (web-grounded)
     on 2026-09-01 for the liquid project. Lightly reformatted only. Note: this topic
     was gathered with the `reason` tier after the deeper `research` tier timed out
     twice; still web-grounded and cited.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Large files and binary assets in version control

## Executive summary

Git's native object model is optimized for text-like, line-oriented history: every committed version is a blob, and a clone normally transfers every reachable version of every file. Git's packfiles can delta-compress related objects, but large, frequently changing, compressed, or already-binary assets are poor delta candidates. A clone therefore has two separate costs:

1. **Object transfer and storage** - all historical blobs unless partial-clone techniques are used.
2. **Working-tree materialization** - files actually checked out on disk.

Git LFS moves large-file contents outside Git's object database, but remains fundamentally **file-versioned**: changing 1 byte of a 10 GB file generally creates and transfers a new 10 GB LFS object. Content-defined chunking systems such as Xet, casync, and backup tools address that limitation by deduplicating within and across file versions.

Perforce Helix Core takes a different approach: a centralized depot, client workspaces that sync selected paths, and first-class exclusive checkout. That is particularly well matched to artists working on unmergeable assets, even though it is less attractive than Git for distributed, branch-heavy source-code development.

---

## 1. Git LFS

### 1.1 Pointer files

With LFS tracking enabled through `.gitattributes`, Git stores a small text pointer instead of the binary payload:

```text
version https://git-lfs.github.com/spec/v1
oid sha256:<content hash>
size <decimal byte count>
```

The pointer is an ordinary Git blob and is included in commits, trees, branches, diffs, and merges. The actual file is stored in the LFS object store under its SHA-256 object ID. The pointer contains the object ID and byte size, which are also the identifiers supplied to the LFS transfer API. [web:3][web:8]

Consequence: Git history can be cloned without the binary data, but a usable working tree requires an LFS-aware client or another mechanism to retrieve and materialize the payload.

### 1.2 Clean/smudge and process-filter architecture

```text
working file
    | git add
    v
clean filter
    |-- hashes and stores the real content in the local LFS cache
    +-- emits a pointer file to Git
    v
Git index / Git object database
```

During checkout:

```text
Git pointer blob
    | checkout
    v
smudge filter
    |-- looks for the object in .git/lfs/objects
    |-- downloads it if absent
    +-- writes the real binary to the working tree
```

Modern LFS normally uses the long-lived `git-lfs filter-process`, which communicates with Git over Git's filter-process protocol rather than starting a new process for every file. [web:1][web:7]

Useful controls:

```bash
git lfs install --skip-smudge
git lfs pull --include='path/to/file'
git lfs fetch --include='path/**'
```

`lfs.fetchinclude` and `lfs.fetchexclude` are **path-selection controls**. They can prevent selected LFS objects from being downloaded, but they do not retrieve arbitrary byte ranges from inside one LFS object. [web:1]

### 1.3 The LFS Batch API

The Batch API is a control-plane protocol:

```json
{
  "operation": "download",
  "transfers": ["basic"],
  "objects": [{"oid": "sha256:...", "size": 123456789}]
}
```

The server responds with an entry for each object, supplying a time-limited `download` action (usually a presigned URL and headers), or an `upload` action optionally followed by `verify`. The client then performs the actual transfer with HTTP `GET`/`PUT`. [web:106][web:108]

```text
Git client -- POST /objects/batch --> LFS server
Git client <- URLs, auth, status ---- LFS server
Git client -- GET/PUT payload ------> object storage
```

The separation is operationally useful: the LFS service authorizes transfers while S3 or Azure Blob Storage carries the payload. It also creates a dependency on the server's LFS implementation, authentication, URL-expiration behavior, and storage policy.

### 1.4 Why LFS is widely disliked

The criticism is not that LFS fails to store large files. It is that it adds a second, file-oriented storage system while retaining Git's repository and workflow complexity.

| Problem | Practical consequence |
|---|---|
| **Server and hosting lock-in** | Git's repository is portable, but useful binary history additionally depends on an LFS endpoint, credentials, object-store retention, and compatible transfer implementation. |
| **GitHub quota and cost** | GitHub Free and Pro currently include 10 GiB of LFS bandwidth and 10 GiB of storage per account; Team and Enterprise Cloud include 250 GiB of each. Excess storage and downloads are billable, and a zero overage budget can block LFS use for the rest of the billing month. [web:5] |
| **Clone can produce only pointers** | Without LFS installed, or when quota/access fails, a clone may contain pointer files rather than usable assets. GitHub explicitly documents pointer-only retrieval when LFS storage limits are exceeded without a payment method. [web:5][web:12] |
| **Whole-file revisioning** | A small edit to a multi-gigabyte file normally uploads another complete LFS object. LFS deduplicates by file object, not by internal chunks. [web:52][web:54] |
| **No general partial fetch** | `fetchinclude`/`fetchexclude` select files or paths; they do not fetch only the changed regions of one file. |
| **History migration is disruptive** | `git lfs migrate import` rewrites commits and changes commit IDs. It must be coordinated with every clone and remote branch. [web:111][web:112] |
| **Locking is opt-in** | LFS file locking exists, but must be configured and used; it is not the universal checkout gate Perforce provides. |
| **Pruning is local-cache housekeeping** | `git lfs prune` removes locally cached objects, optionally checking the remote has a copy. It is not a server-side history garbage collector. [web:110] |
| **Large working-tree cost remains** | Once checked out, a file occupies its full size locally. LFS reduces Git-object transfer, not artist workspace size. |

LFS remains reasonable when files are large but relatively stable and the team wants ordinary Git workflows. It is much less attractive for frequently revised multi-gigabyte checkpoints, datasets, game scenes, or archives.

---

## 2. git-annex

`git-annex` also commits small metadata into Git while storing file contents separately, but is more explicitly a **distributed content-addressed data-management system** than a transparent Git checkout filter.

A repository records keys describing content; actual annexed data may exist in local repositories, SSH remotes, cloud backends, removable drives, or several of them:

```bash
git annex get path/to/file
git annex drop path/to/file
git annex copy path --to=remote
```

`git annex get` retrieves content from a remote or key-value store only when requested. [web:43]

- **Strengths:** many remotes, offline and removable-media workflows, explicit availability, selective retrieval, distributed control, content-addressed storage.
- **Weaknesses:** more concepts and commands, less transparent integration with mainstream Git hosting, weaker interoperability with Git GUI/CI workflows.
- **Important distinction:** git-annex generally treats files as content objects; it is not automatically a chunk-level deduplicating update protocol in the way Xet or a CDC backup repository is.

---

## 3. Git partial clone, sparse-checkout, sparse index, Scalar

### 3.1 Partial clone

```bash
git clone --filter=blob:none --no-checkout <url>
```

A blobless clone transfers commits and trees but omits file-content blobs until Git needs them, fetching from the configured promisor remote on checkout. Git documents `blob:none` as omitting all blobs and `blob:limit=<size>` as omitting blobs at or above a size threshold. [web:31][web:32]

This is not the same as LFS: it works with ordinary Git blobs, preserves Git's object model, and fetches missing objects on demand - but still treats a blob as indivisible (no byte ranges within a blob) and depends on a promisor server, with limitations such as a single promisor remote in the documented model. [web:15]

### 3.2 Sparse-checkout

```bash
git sparse-checkout init --cone
git sparse-checkout set engine tools
```

Sparse-checkout restricts the working tree to selected directories. By itself it does **not** make the object database small. Combined with a partial clone, it avoids both downloading and materializing most irrelevant file contents. [web:33][web:38]

### 3.3 Sparse index

A normal Git index has one entry per tracked file, even when most files are outside the sparse working tree. A sparse index collapses excluded directories into directory entries and expands them only when required, substantially reducing index size and improving `git status`/`git add` - although compatibility with commands and non-cone patterns has historically been imperfect. [web:39][web:165]

### 3.4 Scalar

Scalar is a repository-management layer for very large Git repositories. It configures partial-clone settings, sparse-checkout, filesystem monitoring, commit graphs, prefetch, incremental repacking, and background maintenance. Its default clone obtains commits and trees, enables sparse-checkout, and initially materializes only top-level files unless `--full-clone` is selected. [web:151][web:155]

Scalar is an operational bundle around Git's scalable features, not a new storage format. It does not provide Perforce-style locking or Xet-style internal chunk deduplication.

---

## 4. Perforce Helix Core

### 4.1 Why binary-heavy studios still use it

```text
central depot
    |-- client workspace for programmer
    |-- client workspace for artist
    +-- client workspace / edge server for remote site
```

A client workspace can sync selected depot paths rather than cloning every historical version. For a multi-terabyte asset depot that distinction is fundamental: an artist can sync the current branch and relevant directories without obtaining the entire historical repository.

Helix stream depots provide structured branching rather than Git's free-form reference-and-tree model. [web:153] Perforce also supports proxies close to remote users; a proxy near a WAN client can cache and serve depot content while connecting back to the main server. [web:158] For larger distributed installations, commit, edge, proxy, and workspace-server arrangements place metadata and content operations nearer to teams; some operations, especially global locks, still require communication with the commit server. [web:25]

### 4.2 Exclusive checkout and locking

Perforce's `+l` file type is an exclusive-open policy:

```bash
p4 reopen -t binary+l path/to/scene
```

Only one user can open the file for edit. This is stronger than a lock at submission time: `+l` prevents another user from opening the file at all. [web:17]

Why this matters for binary assets:

- Photoshop, Maya, Blender, Unreal, CAD, and EDA database files are frequently not meaningfully mergeable.
- Two artists can otherwise edit the same base asset and discover the conflict only at submission time.
- A visible checkout state communicates ownership directly in P4V or the command line.
- Revert or submit releases the lock.

Distributed Helix configurations can maintain global exclusive locks in the shared commit server, and all personal servers or edge clients must respect them. [web:16]

### 4.3 What it does better than Git for artists

| Artist requirement | Helix Core advantage |
|---|---|
| Work on a large binary without downloading its history | Sync selected current files from a depot workspace. |
| Prevent two people editing an unmergeable asset | Native exclusive-open (`+l`) is a first-class workflow. |
| Work with a multi-terabyte asset library | Centralized depot and selective sync avoid Git's full-history clone. |
| Operate across studios or WANs | Proxies and edge-server topologies cache content close to users. |
| Organize release/mainline/feature asset flows | Stream depots provide structured branching. |
| See who owns an asset | Checkout state is explicit rather than a social convention. |

Trade-offs: centralized administration, commercial licensing and infrastructure, weaker offline/distributed semantics, and less natural source-code branching/merging. Game and EDA teams tolerate those because the dominant collaboration unit is a large, unmergeable binary artifact, not a small text patch.

---

## 5. Content-defined chunking

### 5.1 Basic principle

Fixed-size chunking breaks when bytes are inserted near the beginning of a file: every subsequent fixed block shifts, destroying deduplication. Content-defined chunking (CDC) chooses boundaries from the data itself:

```text
stream -> rolling fingerprint -> boundary condition -> variable-size chunks
```

After an insertion or deletion, chunk boundaries tend to re-synchronize after a short distance, so unchanged regions retain their hashes.

### 5.2 Rabin fingerprinting

Rabin chunking computes a rolling polynomial fingerprint over a window in GF(2). A boundary is selected when selected fingerprint bits meet a mask condition, subject to minimum and maximum chunk sizes. Because the rolling hash updates as bytes enter and leave the window, the stream need not be rescanned. Restic's chunker uses Rabin fingerprinting over GF(2). [web:130]

### 5.3 Gear hashing and FastCDC

Gear hashing uses a table-driven rolling hash and is generally cheaper than a polynomial Rabin update. FastCDC adds a fast Gear-based rolling hash, a simplified boundary test, skipping cut points below the minimum size, normalized chunk-size distribution, and processing optimizations such as advancing two bytes at a time.

The published evaluation reports FastCDC as **3-12x faster** than prior CDC approaches while achieving nearly the same or higher deduplication ratio than classic Rabin CDC. A related evaluation reports roughly **2,000-2,700 MB/s** for FastCDC on tested data, versus **200-220 MB/s** for the tested Rabin-style methods. [web:68][web:72] The exact result depends heavily on CPU, implementation language, average chunk size, input entropy, compression, and whether encryption is included.

### 5.4 Backup-system strategies and measured deduplication

There is no universal "deduplication ratio" for these tools. It depends on dataset, snapshot interval, churn, chunk parameters, compression, and the definition used. The following are implementation characteristics and one published 500 GB benchmark, not guarantees.

| System | Chunking strategy | Typical/default scale | Published measured result |
|---|---|---:|---:|
| **restic** | Rabin-fingerprint CDC; chunks commonly around 1 MiB. [web:122] | ~1 MiB average | 500 GB, seven-daily-backup benchmark: 1:3.2 logical-to-stored. [web:62] |
| **BorgBackup** | Buzhash CDC with configurable min/target/max/mask. Documented default `buzhash,19,23,21,4095`. [web:128] | ~2 MiB target; configurable | Same benchmark: 1:3.8. [web:62] |
| **Kopia** | CDC with configurable chunking, plus compression and a parallelized repository design. [web:122] | Configuration-dependent | Same benchmark: 1:4.1. [web:62] |
| **Duplicacy** | Buzhash CDC; common defaults ~128 KiB min, 256 KiB average, 512 KiB max in one documented comparison. [web:126] | ~256 KiB average | No comparable independent ratio should be inferred; smaller chunks generally improve resynchronization at metadata and request cost. |
| **casync** | Buzhash CDC over a serialized file tree or block stream; SHA-256 identifies chunks and xz compresses them. Removes file boundaries before chunking, so small files can be grouped and large files split. [web:123] | Configuration-dependent | Architecture documented; no single universal ratio. |
| **desync** | Similar content-addressed, chunk-indexed image distribution model to casync. | Configuration-dependent | Workload-specific; evaluate as an image distribution system. |

The practical design tension:

- **Smaller chunks:** better recovery after insertions and better deduplication, but more hashes, metadata, index entries, and object-store requests.
- **Larger chunks:** lower overhead and often higher throughput, but less effective deduplication for small edits.
- **Compression/encryption order matters:** convergent chunk identity normally must be established before encryption if identical plaintext is to deduplicate; randomized encryption otherwise destroys equality.

---

## 6. Delta encoding for large binaries

CDC and delta encoding are related but different:

- **CDC:** split content into reusable chunks and store a manifest of chunk IDs.
- **Delta encoding:** describe a new version using copies and inserts relative to a specific old version.

### 6.1 xdelta3

`xdelta3` uses a VCDIFF-style, windowed approach. It can process large inputs without requiring the entire old and new files in RAM, making it more suitable than classic `bsdiff` for large objects. Its trade-off is that it may produce a larger patch than a more memory-intensive suffix-array-based algorithm.

Effective when the exact old version is known, the new version shares long byte sequences with it, and the client can retain the old file while applying the patch. Less useful as a general multi-version repository representation because a patch chain can become expensive to apply and depends on a particular base version.

### 6.2 bsdiff/bspatch

The original bsdiff implementation is exceptionally memory-hungry:

- `bsdiff`: approximately `max(17*n, 9*n+m) + O(1)` bytes, where `n` is old-file size and `m` is new-file size.
- `bspatch`: approximately `n+m+O(1)` bytes.

The source documentation also describes bsdiff's O((n+m) log n) generation time and bspatch's O(n+m) application time. [web:76]

A 1 GiB old file can therefore require on the order of **17 GiB** merely for the nominal bsdiff memory formula, before process overhead. Android's bsdiff-derived tooling added lazy/extents-based reading to reduce apply-side memory; the AOSP change notes the older implementation unnecessarily loaded the old file and was expected to cut bspatch memory usage nearly in half. [web:90]

### 6.3 Google Courgette and Zucchini

Chrome updates motivated specialized executable patching because ordinary binary diffs are confused by changes in code layout, relocations, and addresses.

- **Courgette** used executable-aware transformations to normalize aspects of binaries before differencing, making Chrome patches much smaller than generic binary diffs.
- **Zucchini** is the newer Chromium component for PE, ELF, DEX, and related executable images. It identifies executable elements, finds equivalences between old and new binaries, and emits structured raw/reference delta data. [web:171]

Zucchini's documentation reports that patch generation can take approximately **15-20 minutes and 1 GB of RAM for Chrome**, while emphasizing this is a server-side, run-once release operation; patch application is client-side and subject to stricter RAM, disk, and fault-tolerance constraints. [web:171]

This illustrates an important production pattern: spend substantial compute during release generation to save every client a large download, but retain a full-download fallback if the base version is unavailable or patching fails.

### 6.4 zstd `--patch-from` and long-range matching

```bash
zstd --patch-from=old.bin new.bin -o patch.zst
zstd -d --patch-from=old.bin patch.zst -o new.bin
```

This uses the old file as a dictionary and automatically selects parameters suitable for a reference larger than the ordinary dictionary limit, combining dictionary compression with long-range matching; the documented dictionary limit was raised to 2 GiB, and long-distance matching can use windows up to 2 GiB. [web:167][web:179]

Attractive when the old version is available locally and fast decompression with moderate patch generation matters more than the absolute smallest patch. Not a substitute for CDC when many unrelated versions and cross-file deduplication are required.

### 6.5 Android A/B OTA deltas

Android A/B updates place an update payload on the inactive slot while the device continues running the active slot. The payload describes operations that transform source partitions into target partitions; the same framework supports full and delta updates, and delta operations can reference source blocks and may include BSDIFF-derived operations. [web:79][web:82]

A source-based incremental payload is much smaller, but requires the device to be on the exact expected source build. One Android OTA description gives representative ranges of **50-200 MB for a delta payload versus 2-4 GB for a full payload**, while noting the exact-source requirement. [web:82] Android also supports streaming `payload.bin` directly from a package server rather than downloading the complete OTA archive first. [web:84]

---

## 7. Hugging Face Xet/XetHub

Hugging Face acquired XetHub and introduced Xet as a storage backend designed for large AI and dataset files. The Hub still supports legacy Git LFS clients, but Xet-aware clients use a different path. [web:52][web:54]

### 7.1 Architecture

```text
large file
   | Xet-aware client
   v
CDC into ~64 KiB variable chunks
   |
   |-- hash each chunk
   |-- query which chunks already exist
   +-- group new chunks into larger upload blocks
                       |
                       v
                 CAS / object storage
```

Current documentation describes: target chunk size around **64 KiB**; roughly **8 KiB minimum** and **128 KiB maximum**; cryptographic chunk identity; grouping new chunks into approximately **64 MiB blocks** to reduce per-chunk metadata and request overhead; content-addressed storage of those blocks; and reconstruction metadata for downloads. [web:47][web:52][web:55]

During download, the client obtains the chunk/range manifest and retrieves required ranges from storage, potentially in parallel. A local chunk cache avoids fetching overlapping data repeatedly. [web:50][web:60]

The result is Git-compatible at the repository metadata level, but the large-file backend is closer to a chunked CAS than to an LFS file store.

### 7.2 Reported gains

Hugging Face's CORD-19 benchmark compared a Git-LFS-backed repository with a Xet-backed repository over 50 incremental dataset updates:

| Metric | Git LFS | Xet |
|---|---:|---:|
| Average download time | 51 minutes | 19 minutes |
| Average upload time | 47 minutes | 24 minutes |
| Storage used | 8.9 GB | 3.52 GB |

The same post describes this as a consistent ~50% improvement in storage and transfer performance across three iterative-development use cases. [web:46]

For two versions of GPT-2 `model.safetensors`:

| Versions | Git LFS | Xet |
|---|---:|---:|
| Version 1 | 664 MB | 509 MB |
| Version 2 | 548 MB | 136 MB |
| Total | 1.2 GB | 645 MB |

A reported **53% total-storage reduction** before an estimated additional 10% from compression. [web:46]

Appending 1 MB to a 5 GB SQLite database: LFS re-uploads approximately 5 GB (about 13 minutes at 50 Mb/s); Xet transfers only the new data, with reported upload time about 0.1 seconds at that link speed. [web:49]

These results are especially plausible for model checkpoints with mostly stable tensor regions, append-heavy datasets, Parquet or database files with localized modifications, and forks or revisions sharing substantial binary content. They are less impressive for encrypted or fully recompressed files where a small logical edit changes nearly every byte. Hugging Face reports observed deduplication ranges of roughly **30-85%**. [web:46]

### 7.3 Limitations

- A checkout still materializes the complete requested file.
- Very small chunks increase index and metadata overhead.
- A client must understand the Xet path for best behavior; legacy clients use an LFS bridge. [web:54]
- Current Hub documentation recommends a per-file size limit - pages have reported both 20 GB and 200 GB depending on version and backend policy - so check the current service limit rather than assuming arbitrary multi-terabyte single-file support. [web:47][web:52]

---

## 8. DVC and ML checkpoint versioning

DVC keeps Git for code, configuration, experiment metadata, and small pointer files, while storing datasets and models in a DVC cache and configurable remote storage (S3, Azure, GCS, SSH, local server):

```bash
dvc add data/train.parquet
git add data/train.parquet.dvc data/.gitignore
git commit -m "Update training data"
dvc push
```

To reproduce: `git checkout <commit>` then `dvc checkout`. The `.dvc` file records content identity and metadata; large data is not placed in Git. DVC documents this as versioning data and models through Git-tracked metafiles and external storage, with a cache that avoids repeatedly copying identical content. [web:92][web:104]

DVC is best understood as **Git-linked artifact versioning**, not a replacement for Git: strong for reproducible datasets, model checkpoints, pipelines, metrics, and experiment state; storage-backend agnostic; usually file/object oriented unless the selected remote adds its own chunking.

| Requirement | Better fit |
|---|---|
| Reproduce exact code/data/model combinations | DVC |
| Share Git-like repository metadata and revisions | DVC or Xet |
| Transfer only changed regions of a large checkpoint | Xet/CDC or a checkpoint-aware delta format |
| Serve a model artifact simply to consumers | Object store, registry, or Hub |
| Prevent two artists editing one scene | Perforce |

---

## 9. Concrete pain reports

| Report | Numbers | What it demonstrates |
|---|---:|---|
| Dropbox monorepo cleanup | Repository reduced from **87 GB to 20 GB**; clone time fell from **over 1 hour to under 15 minutes**; the tuned repack took approximately **9 hours**. [web:136] | Pack layout and historical object organization can dominate both server maintenance and clone time. |
| Chromium-scale repack discussion | A reported repository was approximately **9.5 GB** on initial full clone; local repacking reduced it to approximately **1.5 GB** but took several hours. Another path reported about **15.5 hours** for a large repack, versus roughly three days previously. [web:139] | Pack compression can save several times the disk space, but may consume many hours of CPU. |
| Large repository with poor packing | Fresh clone originally took **2-3 hours**; limiting pack memory reduced it to about **15 minutes**. Disabling binary deltas then produced approximately **20-minute** clones. [web:138] | Memory pressure and swapping during server-side packing can dominate apparent network performance. |
| GitHub monorepo maintenance | GitHub described maintenance triggers after every **50 pushes** or **40 MB of unpacked files**; normal maintenance ranged from seconds to minutes, and repack tuning eliminated nearly all reported maintenance failures. [web:137] | Hosted Git providers must actively engineer pack maintenance; large repositories are not "set and forget." |
| `go-git` large-clone comparison | For a repository with approximately **110,000 commits** and **1.5 GB unpacked**, `go-git` used up to **5 GB RAM** and took over **4 minutes**, while native Git used approximately **290 MB** and about **1 minute**. [web:149] | Implementation details matter: a Git-compatible object model does not guarantee comparable memory behavior. |
| Generic Git repack memory | One maintenance analysis estimates roughly **150 bytes of heap per object** during repacking. [web:94] | Object count - not merely payload bytes - can become the limiting factor for very granular histories. |

### What these numbers imply

1. **"The repository is only 20 GB after repacking" does not mean it is cheap to maintain.** The repack itself may require hours and substantial temporary disk and RAM.
2. **Binary deltas are not automatically beneficial.** If binary objects are unrelated or compressed, delta search consumes CPU/RAM without reducing much data. Disabling deltas can make packing faster and more predictable at the cost of disk space.
3. **Partial clone attacks transfer cost, not all local cost.** Once a build, artist tool, or test touches many paths, the missing blobs arrive.
4. **LFS attacks Git pack growth but not revision churn.** A frequently revised 20 GB file can still produce enormous external storage and bandwidth bills.
5. **CDC attacks revision churn, but introduces chunk-index and cache complexity.**
6. **Perforce attacks the workflow problem directly.** Artists sync what they need and lock what cannot be merged, while the depot remains the authoritative multi-terabyte store.

## Bottom line

- Use **Git + LFS** for moderate binary assets and teams already committed to Git hosting.
- Use **Git partial clone + sparse-checkout + sparse index + Scalar** for large source monorepos where the main problem is unnecessary history/blob transfer.
- Use **Perforce Helix Core** when the main problem is a large, centralized, artist-heavy or EDA-heavy depot with unmergeable files and mandatory checkout ownership.
- Use **CDC/CAS systems** such as Xet, casync, or appropriately configured backup stores when revisions share internal regions and file-level deduplication wastes bandwidth.
- Use **delta encoders** for controlled old-version-to-new-version updates, especially software and OTA delivery - not as a universal repository storage model.
- Use **DVC** to bind large datasets and ML artifacts reproducibly to Git commits, with Xet or object storage underneath when chunk-level transfer efficiency matters.

## Citations

[1] https://man.archlinux.org/man/extra/git-lfs/git-lfs-filter-process.1.en
[3] https://docs.rhodecode.com/4.x/rce/tutorials/git-lfs-ext.html
[5] https://docs.github.com/en/billing/concepts/product-billing/git-lfs
[7] https://docs.syntevo.com/SmartGit/Latest/Manual/GitConcepts/GitLargeFileStorage
[8] https://github.com/git-lfs/git-lfs/blob/main/docs/extensions.md
[12] https://docs.github.com/en/enterprise-cloud@latest/repositories/working-with-files/managing-large-files/about-storage-and-bandwidth-usage
[15] https://git-scm.com/docs/partial-clone/2.16.6
[16] https://help.perforce.com/helix-core/server-apps/dvcs/2023.2/Content/DVCS/fetching.exclusive.html
[17] https://help.perforce.com/helix-core/server-apps/cmdref/current/Content/P4Guide/resolve.lock.exclusive.html
[25] https://ftp.perforce.com/perforce/r18.2/doc/manuals/p4dist.pdf
[31] https://git-scm.com/docs/git-clone
[32] https://github.blog/open-source/git/get-up-to-speed-with-partial-clone-and-shallow-clone/
[33] https://www.mslinn.com/git/600-partial-clone.html
[38] https://github.blog/open-source/git/bring-your-monorepo-down-to-size-with-sparse-checkout/
[39] https://github.com/git/htmldocs/blob/gh-pages/git-sparse-checkout.html
[43] https://man.archlinux.org/man/extra/git-annex/git-annex-get.1.en
[46] https://huggingface.co/blog/from-files-to-chunks
[47] https://huggingface.co/docs/hub/en/xet/deduplication
[49] https://huggingface.co/blog/xet-on-the-hub
[50] https://huggingface.co/blog/migrating-the-hub-to-xet
[52] https://huggingface.co/docs/hub/main/storage-backends
[54] https://huggingface.co/docs/hub/xet/legacy-git-lfs
[55] https://huggingface.co/docs/xet/en/deduplication
[60] https://huggingface.co/docs/hub/en/xet/using-xet-storage
[62] https://createinnovateexplore.com/borg-restic-and-kopia-benchmarks-on-a-500gb-dataset/
[68] https://www.computer.org/csdl/journal/td/2020/09/09055082/1iHrr9y8mje
[72] https://cs.uwaterloo.ca/~alkiswan/papers/SeqCDC_Middleware24.pdf
[76] https://www.daemonology.net/bsdiff/
[79] https://source.android.com/docs/core/ota/ab
[82] https://aospbooks.github.io/aosp-internal-book/55-ota-updates/
[84] https://android.googlesource.com/platform/bootable/recovery/+/469b35a/updater_sample/
[90] https://android.googlesource.com/platform/external/bsdiff/+/99b5374%5E!/
[92] https://doc.dvc.org/example-scenarios/versioning-data-and-models
[94] https://monorepo-book.github.io/maintenance-failure.html
[104] https://doc.dvc.org/example-scenarios/versioning-data-and-models/tutorial
[106] https://github.com/git-lfs/git-lfs/blob/main/docs/api/basic-transfers.md
[108] https://github.com/git-lfs/git-lfs/blob/main/docs/api/README.md
[110] https://github.com/git-lfs/git-lfs/blob/main/docs/man/git-lfs-prune.adoc
[111] https://manpages.ubuntu.com/manpages/focal/man1/git-lfs-migrate.1.html
[112] https://man.archlinux.org/man/git-lfs-migrate.1.en
[122] https://www.opensourcegroups.com/open-source-backup/borg-vs-restic.php
[123] https://0pointer.net/blog/casync-a-tool-for-distributing-file-system-images.html
[126] https://forum.duplicacy.com/t/deduplication-vs-compression-and-whats-the-difference-between-rolling-chunker-and-fixed-block-size/6325
[128] https://borgbackup.readthedocs.io/en/1.2.5/usage/notes.html
[130] https://deepwiki.com/restic/chunker/1.1-key-concepts
[136] https://dropbox.tech/infrastructure/reducing-our-monorepo-size-to-improve-developer-velocity
[137] https://github.blog/developer-skills/github/improving-large-monorepo-performance-on-github/
[138] https://stackoverflow.com/questions/13367431/issues-after-trying-to-repack-a-git-repo-for-improved-performance
[139] https://news.ycombinator.com/item?id=41960967
[149] https://github.com/src-d/go-git/issues/447
[151] https://git-scm.com/docs/scalar
[153] https://help.perforce.com/helix-core/server-apps/p4sag/current/Content/P4SAG/depots.stream.html
[155] https://github.com/microsoft/git/blob/HEAD/contrib/scalar/docs/index.md
[158] https://ftp.perforce.com/pub/perforce/r18.2/doc/manuals/overview/Content/Overview/basic_concepts.performance.proxy.html
[165] https://github.com/git/git/blob/master/sparse-index.h
[167] https://github.com/facebook/zstd/wiki/Zstandard-as-a-patching-engine
[171] https://chromium.googlesource.com/chromium/src/+/HEAD/components/zucchini/
[179] https://github.com/facebook/zstd/releases/tag/v1.3.2
