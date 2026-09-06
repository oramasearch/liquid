# Consumer sync and web

> Research: [21-consumer-sync-and-web](../research/21-consumer-sync-and-web/)
>
> **Requirement:** a Dropbox/Google Drive-style interface so regular people can use
> liquid, on the web and locally.

> **Status:** liquid #195 implements the cross-platform drive client: persisted remote, local,
> and base trees; deterministic reconciliation; named conflict values; resumed sync transfer;
> path history and restore; CLI one-shot and daemon-hosted watch modes. Liquid #235 adds Linux
> FUSE online-only, available-offline, and excluded states; persistent pinning; lazy hydration;
> offline write queues; and GNOME Files and Dolphin badges. Windows CFAPI, macOS File Provider,
> and lake's web drive remain consumer work in lake #45 and #43.

## Why this is nearly free

Dropbox's architecture is structurally what liquid already is:

| Dropbox | liquid |
|---|---|
| Files split into content-addressed 4 MiB blocks, SHA-256 | Files split into content-defined FastCDC chunks, BLAKE3 |
| A file version is an ordered **blocklist** | A file version is an ordered chunk list |
| **Block service** — dumb hash → encrypted block store | The chunk store ([01](01-object-model-and-storage.md)) |
| **Metadata service** — namespace, paths, revisions, permissions, journal | The CRDT datastore ([17](17-implementation.md)) |
| Transfers only blocks the destination lacks ("delta sync") | Transfers only missing chunks |

liquid's content-defined chunking is **strictly better than fixed 4 MiB blocks**: an
insertion near the start of a file shifts every subsequent fixed block, while CDC
resynchronises within a chunk or two.

The metadata layer is what liquid adds — and Automerge is exactly the right shape for
it, because a shared namespace is a concurrently-edited structured document.

## The sync engine

**The three-tree model**, from Dropbox's Nucleus rewrite:

- **Remote tree** — server state.
- **Local tree** — observed disk state.
- **Base tree** — last mutually synchronised state, acting as a per-node merge base.

Comparing all three reveals whether a path changed locally, remotely, on both sides,
or merely moved.

**Stable identity solves renames.** A stateless scanner sees a rename as delete plus
create. Automerge's `ObjId` gives every path a stable identity that survives moves —
which is precisely the deficiency that forced Dropbox to rewrite its engine, since
representing a move as delete+add could lose data.

**Filesystem events are hints, not truth.** inotify queues overflow, FSEvents
coalesces to directory granularity, mounts vanish. liquid persists a cursor and can
always rescan against its database. Treating the event stream as authoritative is the
classic sync bug.

**Deterministic, seeded simulation.** Nucleus's real lesson is testability: run the
reconciliation state machine deterministically so a simulator can explore the state
space reproducibly. For a system holding people's files, this matters more than
throughput ([17](17-implementation.md)).

## Local: files that are not there

liquid presents a virtual filesystem using each platform's native mechanism — the
same materialiser that serves developer checkouts
([01](01-object-model-and-storage.md)). One component, two products.

| Platform | API | Support |
|---|---|---|
| **Windows** | Cloud Files API (CFAPI) | Full — NTFS placeholders, hydration callbacks, dehydration, three pin states, **native Shell badges** |
| **macOS** | File Provider extension | Full — system-driven materialisation, `evictItem`, LRU eviction, Finder decorations. Kernel extensions are deprecated, so this is the only path |
| **Linux** | FUSE | Implemented by liquid #235 with repository-local state, xattrs, hydration policy, and separate GNOME Files and Dolphin providers. |

Linux is real work and is budgeted as such.

Users get three states, and liquid keeps the distinction clear — Dropbox's separate
"online-only" and "selective sync" models are a documented source of confusion:

- **Online-only** — visible, hydrates on open.
- **Available offline** — pinned locally.
- **Not synced here** — absent from this device's namespace entirely, still in the
  cloud and still shared.

## Correctness details that lose data

- **Atomic publication**: write to a temp file on the same filesystem, verify chunk
  hashes, rename over the destination — and `fsync` file *and parent directory* when
  durability is required. Atomic visibility is not the same as crash durability.
- **Partial uploads**: chunks first, then a metadata commit. A half-uploaded file is
  never visible.
- **Names**: case, Unicode normalisation form and path length checked against both the
  canonical rules and the destination filesystem *before* publication. Two entries
  collapsing to one local name is an error or a deterministic rename — **never a
  silent overwrite**.
- **Files being written**: debounce, check size/mtime stability, hash a consistent
  snapshot.
- **Locked destinations**: retain pending state and retry; never treat "temporarily
  inaccessible" as deletion.
- **Delete vs edit** is a real conflict, not automatically a delete.
- **Authorisation checked at commit**, not at open.

## Conflicts, presented properly

The CRDT layer guarantees convergence and that no data is lost. For file *content*
there is still no safe generic merge, so both versions are preserved.

Where every other product then shows a mysteriously named duplicate file — which
users delete without comparing — liquid presents a conflict as **"two versions of
this file, here is what differs"**, using structural diff
([03](03-diff-and-merge.md)). For a spreadsheet or a document that is a meaningful
answer rather than a filename suffix.

## Version history is the feature people actually want

Every version is already stored as chunks; every change is already in the CRDT
history with time travel ([17](17-implementation.md)).

"Show me last Tuesday's version" is a **query**, not a backup-restore operation. So is
"who changed this, and what did they change?"

## Sharing, designed against the known failures

Research on non-technical users is blunt: they hold a "muddy" model of access rights,
rarely inspect effective permissions, and cannot predict how move, copy and
email-link operations affect collaborators.

At the point of sharing, liquid shows: **who can access, what they can do, whether
access is inherited, and whether the recipient gets a live object, a link, or an
independent copy.** Not behind a settings dialog.

Specific confusions designed against:

- A share link is a **continuing capability**, not an attachment.
- "Editor" silently including delete and reshare — so permissions are named for what
  they permit.
- Permissions inherited from **location**, so moving a file changes its audience —
  stated at the moment of the move.
- Un-syncing a folder locally does **not** revoke anyone's access.
- A downloaded copy cannot be recalled.

## Web

Browse, preview, search ([12](12-search.md)), share, and view history. Preview uses
the format registry ([03](03-diff-and-merge.md)) — the same code that decomposes a
`.deb` renders a spreadsheet.

## Encryption

Available per subtree ([11](11-secrets-and-encryption.md)), with the cost stated
plainly: end-to-end encrypted content cannot be server-side searched, previewed,
scanned or recovered. Dropbox's encrypted team folders lose shared links, previews,
search and API access for the same reason. That is a real trade, and the UI says so
rather than burying it.

## Open questions

- **Whether consumer and developer namespaces are the same objects** or two views
  over one store. Probably the latter, but the permission models differ substantially.
- **CRDT history growth** for a namespace with millions of files over years — the
  same open risk as [17](17-implementation.md), and it lands hardest here.
- **Real-time collaborative editing** of documents. Automerge makes it possible; it
  is a large product surface and is not in scope initially.
