# Object model and storage

> Research: [02-large-files](../research/02-large-files-and-binary-assets/) ·
> [12-artifact-distribution](../research/12-artifact-distribution/) ·
> [01-version-control](../research/01-version-control/)

Everything in liquid is built on one store. Getting this layer right matters more
than any other decision, because it cannot be changed later.

## Chunks

Every byte that enters liquid — source file, compiled object, model checkpoint,
`.deb`, container layer, search index segment — is split by **FastCDC** content-
defined chunking and stored by content hash.

- Target chunk size **64 KiB**, minimum 8 KiB, maximum 128 KiB.
- Hash: **BLAKE3** by default, with the algorithm identifier encoded in every reference.
  SHA-256 can be selected where an ecosystem requires it, and both forms can coexist in one
  store. [Hash agility](../library/hash-agility.md) records the current behavior and limits.
- Chunks are the **deduplication and identity** unit.

There is no "large file mode", no pointer files, and no separate LFS server. A
40 GB dataset and a 200-byte header are stored by the same mechanism. This is the
single most important consequence of the design: large-file support is not a
feature, it is an absence of a limitation.

## Packs

Chunks are grouped into **packs** of roughly 16–64 MiB for storage and transfer.

- Packs are the **transfer and storage** unit; chunks are the identity unit.
- A pack is fetched by HTTP range request, so a client retrieves only the byte
  ranges holding chunks it lacks.
- Packs are compressed with **zstd**, with `--rsyncable` enabled by default.

Keeping these two units distinct is not an optimisation — a store that treats
chunks as objects dies of request overhead at any real scale.

## Two layers: metadata and content

liquid separates the **namespace** from the **bytes**, and this boundary is the most
important structural decision after chunking itself.

| Layer | Holds | Implementation |
|---|---|---|
| **Metadata** | Namespace, trees, paths, names, refs, branches, change history, permissions, small structured data | The CRDT datastore, [`automerge-cpp`](https://github.com/homer6/automerge-cpp) |
| **Content** | Every byte | The chunk store described here |

Metadata documents reference content **by hash**; they never contain it.

This is exactly Dropbox's metadata-service / block-service split and Seafile's
commit / FS-object / block split — both well-precedented. It is also what keeps a
CRDT viable: Automerge is excellent for a structured document many people edit
concurrently, and entirely wrong as a container for terabytes of binary data.

What the CRDT layer contributes: conflict-free convergent merge, stable object
identities that survive moves, a bloom-filter sync protocol that becomes liquid's
replication protocol, time travel for history, cursors, and incremental change
patches that drive rebuilds and indexing. See
[17-implementation](17-implementation.md) for the full rationale and the open risks —
principally CRDT history growth.

## Trees and nodes

A **tree** is a sorted list of entries: name, mode, type, and either a chunk list
(file) or a tree hash (directory). Trees are themselves chunked and stored, so
directory structure deduplicates alongside content.

Node types:

| Type | Contents |
|---|---|
| `blob` | An ordered chunk list plus total size |
| `tree` | Sorted directory entries |
| `change` | A recorded change with parents, author, message, and identity (see [02](02-version-control.md)) |
| `action` | An input tree, command, environment and declared outputs (see [04](04-build-engine.md)) |
| `result` | An action's outputs, exit status and observed input manifest |
| `attestation` | A signed statement about another node (see [10](10-supply-chain.md)) |
| `repo` | A nested repository with its own history (see [02](02-version-control.md)) |

## The index

The chunk index must scale to **billions of entries**. It maps chunk hash →
(pack, offset, length), plus reference metadata for garbage collection.

Backed by **liquid's own index**, built from scratch (Steve, 2026-09-03): a fixed-width LSM that is
always rebuildable from the packs. The RocksDB/LMDB benchmark
(`../library/chunk-index-benchmark.md`) is the bar it must meet. It is explicitly **not** a directory of files —
that is the design error that makes naive content-addressed stores unusable.

## Lazy materialisation

Nothing is written to a working tree until it is read.

liquid presents a **virtual filesystem** using the platform's native mechanism —
Windows Cloud Files API, macOS File Provider, FUSE on Linux — and hydrates chunks
on page fault. One implementation serves five needs:

1. Source checkout ([02](02-version-control.md))
2. Large-file access ([above](#chunks))
3. Remote build output materialisation ([06](06-distributed-execution.md))
4. Artifact fetch ([09](09-artifact-service.md))
5. Container root filesystems ([13](13-deployment-runtime.md))

A conventional full checkout remains available for tools that cannot tolerate a
virtual filesystem.

**Phase 1 implementation (decision 2026-09-04):** libfuse3 on Linux, optional macFUSE on macOS,
and the conventional full checkout on every platform. macOS File Provider and Windows Cloud Files
require a signed host application and arrive with the drive in Phase 6. Phase 1 remote hydration is
out of scope; the same chunk-range path reads the local store.

## Garbage collection

Designed in from the start, because retrofitting it is not possible.

- **Leased roots.** A root is a lease with an expiry. An abandoned client's roots
  age out rather than pinning storage forever.
- **Epoch-based marking.** Collection proceeds in generations without a global
  stop-the-world pass.
- **Lifetime coupling.** A cached action result and the chunks it references share
  a lifetime. An entry whose chunks have been collected is never returned — the
  "cache hit that cannot be materialised" failure is structurally excluded.
- **Chunk-level reference tracking** cheap enough for billions of objects.

## Integrity

Every chunk is verified against its hash on read. A chunk that fails verification
is a hard error and is reported, never silently refetched and forgotten.

Chunk identity for encrypted content is a **keyed** MAC rather than a plain hash,
so deduplication works without exposing a convergent-encryption oracle. See
[11](11-secrets-and-encryption.md).

## What this buys, concretely

| Scenario | Conventional tool | liquid |
|---|---|---|
| Append 1 MB to a 5 GB file | Git LFS re-uploads 5 GB | Uploads the changed chunks |
| Same vendored zlib inside a wheel, a `.deb` and an OCI layer | Stored three times | Stored once |
| Rebuild changes one function in one object file | New layer, new image, full push | A handful of new chunks |
| Checkout of a 200 GB monorepo | Hours, 200 GB of disk | Immediate; disk grows with what is read |
| Two branches differing in three files | Two full trees | Shared chunks, three new blobs |

## Open questions

- ~~RocksDB vs LMDB~~ **Resolved: neither.** liquid's own index, built from scratch
  (`CLAUDE.md` *Decisions* 8); the benchmark (`../library/chunk-index-benchmark.md`) is the bar.
- **Pack compaction policy.** Packs accumulate dead chunks as history is collected;
  compaction is a write-amplification/space tradeoff not yet decided.
- **Chunk size tuning per content type.** 64 KiB is a reasonable default; source
  code may want smaller, media larger. Whether to vary this by detected type, and
  whether varying it hurts cross-type dedup, is unresolved.
