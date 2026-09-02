# Large files and binary assets

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** large-file support must work from day one, not as a later
> extension. Every design decision in the object model has to assume multi-gigabyte
> files are normal.

## Why Git fails at this

Two independent costs, often conflated:

1. **Object transfer and storage.** A clone normally fetches every reachable
   version of every file. Git's packfiles delta-compress related objects, but
   already-compressed or genuinely-different binaries are poor delta candidates,
   so history accumulates at full size.
2. **Working-tree materialisation.** Even with a perfect transfer story, checking
   out a 40 GB tree writes 40 GB to disk.

Concrete evidence of how badly this scales: Dropbox reduced a monorepo from 87 GB
to 20 GB and clone time from over an hour to under 15 minutes — but the tuned
repack itself took **9 hours**. A Chromium-scale repository took several hours to
repack locally. One team found that *disabling binary deltas entirely* made clones
faster (20 minutes vs 2–3 hours), because delta search on unrelated binaries burns
CPU and RAM for almost no size win. Git repack costs roughly 150 bytes of heap per
object, so object *count* — not payload size — becomes the limiting factor.

## Git LFS is a bad patch, and everyone knows it

LFS replaces the file content with a pointer blob and stores the payload
elsewhere, fetched through clean/smudge filters and a Batch API that hands out
presigned URLs.

The design flaw is simple: **LFS is still file-versioned.** Change one byte of a
10 GB file and you create, upload and later download another 10 GB object. It
deduplicates whole objects, never their contents.

The operational complaints on top of that:

- **Hosting lock-in.** The Git repo is portable; the binary history is not — it
  depends on an LFS endpoint, its auth, and its retention policy.
- **Cost and quota.** GitHub Free/Pro include 10 GiB storage and 10 GiB bandwidth;
  exceeding them without a payment method means clones return *pointer files
  instead of content*. A repository that silently checks out garbage when you hit a
  billing limit is a serious failure mode.
- **Clone is broken without the extension.** A fresh machine without git-lfs gets
  pointers.
- **Migration rewrites history.** `git lfs migrate import` changes every commit ID.
- **No partial fetch within a file.** `fetchinclude`/`fetchexclude` select whole
  files; you cannot fetch the changed region of one file.
- **Locking is opt-in** and therefore usually absent when needed.

## What Perforce gets right

Game studios and EDA teams have not stayed on Perforce out of inertia. Helix Core
solves problems Git does not attempt:

- **Selective sync from a central depot.** A workspace pulls the paths you need at
  the revision you need. There is no "download all history" step, which is what
  makes multi-terabyte depots workable at all.
- **Exclusive checkout (`+l`)** is the decisive feature. Binary assets — Maya
  scenes, PSDs, CAD, EDA databases — are *unmergeable*. `+l` prevents a second user
  from even opening the file for edit, so the conflict is prevented rather than
  discovered at submit time. This is a workflow guarantee Git structurally cannot
  offer, because in a distributed model there is nobody to ask.
- **Proxies and edge servers** cache content near remote studios.
- **Stream depots** give structured branching rather than free-form refs.

The lesson is not "be centralised." It is that **for unmergeable content,
pessimistic locking is the correct concurrency model**, and a system that only
offers optimistic merging is unusable for artists.

## Content-defined chunking: the actual fix

Fixed-size blocks break on insertion — every subsequent block shifts. Content-
defined chunking picks boundaries from a rolling hash of the data itself, so after
an edit the boundaries resynchronise within a chunk or two and every unchanged
region keeps its identity.

| Algorithm | Notes |
|---|---|
| **Rabin fingerprint** | Rolling polynomial hash over GF(2). Used by restic. ~200–220 MB/s in one comparison. |
| **Gear hashing** | Table-driven rolling hash; cheaper per byte than Rabin. |
| **FastCDC** | Gear hash plus normalised chunk-size distribution, cut-point skipping below minimum size, and two-bytes-at-a-time scanning. Reported **3–12× faster** than prior CDC at equal or better dedup ratio; ~2,000–2,700 MB/s measured. **This is the one to implement.** |

Measured whole-system dedup on a 500 GB, seven-daily-backup benchmark: restic
1:3.2, Borg 1:3.8, Kopia 1:4.1. Real but workload-dependent; do not promise a
number.

The tradeoff is fixed: **smaller chunks → better dedup and faster resync after
edits, but more hashes, more index entries, more object-store requests.** Larger
chunks invert all of that.

## Hugging Face Xet: the proof it works in production

Xet replaced Git LFS on the Hugging Face Hub — the largest real-world migration
from file-versioned to chunk-versioned large-file storage. The architecture:

CDC into ~64 KiB chunks (8 KiB min, 128 KiB max) → hash each chunk → ask the
server which chunks are new → **group new chunks into ~64 MiB blocks** before
upload → content-addressed block storage → download via a reconstruction manifest
with parallel range requests and a local chunk cache.

The 64 MiB grouping is the detail worth noting: raw per-chunk objects would mean
millions of tiny requests, so chunks are the *dedup* unit while blocks are the
*transfer and storage* unit. Any implementation must separate these.

Reported results (CORD-19, 50 incremental updates): download 51 min → 19 min,
upload 47 min → 24 min, storage 8.9 GB → 3.52 GB. Two GPT-2 `safetensors`
versions: 1.2 GB → 645 MB, a 53% reduction. Appending 1 MB to a 5 GB SQLite file:
LFS re-uploads ~5 GB (≈13 min at 50 Mb/s), Xet uploads the delta in ~0.1 s.
Observed dedup across the Hub ranges **30–85%** — highly workload-dependent, and
poor for encrypted or fully-recompressed content where a small logical edit
changes every byte.

## Delta encoding is a different tool

CDC splits content into reusable chunks. Delta encoding describes version B
relative to a specific version A. They solve different problems and liquid needs
both.

- **bsdiff** produces excellent patches and is memory-catastrophic:
  `max(17n, 9n+m)` bytes to generate. A 1 GiB input needs ~17 GiB. Unusable as a
  general repository mechanism.
- **xdelta3** is windowed and streams, so it handles large inputs — bigger patches,
  survivable memory.
- **zstd `--patch-from`** uses the old file as a dictionary with long-range
  matching (2 GiB windows). Fast, good enough, and already a dependency liquid
  will have anyway.
- **Courgette / Zucchini** (Chrome) are executable-aware: they normalise
  relocations and addresses so a recompiled binary diffs small. Zucchini spends
  15–20 minutes and 1 GB RAM *per release* to save every client a large download.

**The pattern to copy:** expensive, asymmetric, server-side patch generation is
worth it when a patch is generated once and applied a million times — with a
full-download fallback when the base version is missing or patching fails. Android
A/B OTA shows the payoff: 50–200 MB delta vs 2–4 GB full payload.

## Implications for liquid

1. **Content-defined chunking is the storage primitive, not an add-on.** Every
   file — source, binary, model checkpoint, dataset — enters the store as FastCDC
   chunks. There is no "large file mode" and no pointer files, because there is no
   distinction to make. This is the single most important decision in this
   document, and it must be made before anything else is built.

2. **Separate the dedup unit from the transfer unit.** Chunks (~64 KiB) for
   identity and dedup; packed blocks (~tens of MiB) for upload, storage and CDN
   transfer. Xet's numbers depend on this and a naive implementation without it
   will die of request overhead.

3. **Virtual, lazy working copies.** Never materialise what was not asked for. The
   same machinery serves monorepo scale ([16](../16-monorepo-scale-and-authoring-loop/))
   and build-artifact fetching ([07](../07-distributed-execution/)) — one
   materialiser, three use cases.

4. **Pessimistic locking as a first-class feature.** For unmergeable content,
   optimistic merge is the wrong model. liquid needs Perforce-style exclusive
   checkout, declared per path pattern, and it needs to work in a distributed
   setting — which means a lock is a coordinated claim against a designated
   authority for that path, not a purely local assertion. Skipping this makes
   liquid unusable for exactly the teams whose pain is greatest.

5. **Never break a checkout because of quota, auth or a missing extension.** LFS's
   pointer-file failure mode is disqualifying. If content cannot be fetched, that
   is a loud error, never a silently wrong working tree.

6. **Chunk-aware transfer, delta as an optimisation on top.** Range requests,
   resumable uploads, parallel fetch, local chunk cache. Use `zstd --patch-from`
   where a known base exists; reserve executable-aware delta for the artifact
   service ([12](../12-artifact-distribution/)) where the
   generate-once/apply-many economics hold.

7. **Watch the object-count ceiling.** Git's repack cost is per-object. A chunked
   store has *far* more objects than a file-based one, so the index must be
   designed for billions of chunks from the start — not a flat directory, not one
   file per chunk.
