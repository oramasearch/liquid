# Consumer sync and web

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** a Dropbox/Google Drive-style interface so non-developers can use
> liquid, on the web and locally.

## Why this is a natural fit, not a bolt-on

Dropbox's architecture is, structurally, what liquid already is:

- Files split into **content-addressed blocks** (4 MiB fixed, SHA-256), with a file
  version represented as an ordered **blocklist**.
- A **metadata service** holding users, namespaces, paths, revisions, blocklists and
  a monotonically increasing per-namespace journal ID.
- A **block service** that is a dumb key-value store from hash to encrypted block,
  which deliberately knows nothing about filenames or users.
- Transfer moves only blocks the destination lacks — Dropbox calls this delta sync.

liquid's chunk store is the block service, with content-defined chunking instead of
fixed 4 MiB blocks — which is strictly better for edits that shift offsets. The
metadata layer is what liquid must add.

Seafile is the closest existing design to liquid's: libraries with commits, each
commit pointing to a root filesystem object, directory objects pointing to children,
and file objects holding ordered block lists — explicitly Git-like, with
content-defined chunks averaging ~8 MB.

## The three-tree model

Dropbox's Nucleus rewrite (2020, Rust) is the most valuable single lesson. It
maintains three trees:

- **Remote Tree** — cloud state.
- **Local Tree** — observed disk state.
- **Synced Tree** — the last mutually synchronised state.

The Synced Tree is a **per-node merge base**. Comparing all three reveals whether a
path changed locally, remotely, on both sides, or merely moved. This replaces the
older approach of persisting a queue of intended operations, which could not
represent moves atomically and lost data.

The reasons for the rewrite are worth internalising, because they are all
architectural rather than performance-related: no stable identities across moves;
a move represented as separate delete/add; weak consistency guarantees; and coarse
threading that made failures nondeterministic and untestable. Nucleus prioritised
**strong invariants, stable IDs, atomic moves, and deterministic simulation** —
running the state machine on one control thread specifically so seeded simulation
could explore the state space reproducibly.

## OS integration: the part with no shortcuts

Presenting files that are not there requires platform-specific APIs, and the three
platforms are not equivalent:

| Platform | API | What it gives |
|---|---|---|
| **Windows** | **Cloud Files API / Cloud Filter** (`cldflt.sys`, CFAPI) | Full support. NTFS placeholders, hydration via fetch-data callbacks, `CfDehydratePlaceholder` for eviction, three pin states, and **native Shell badge integration**. |
| **macOS** | **File Provider extension** | Full support. Domain under `~/Library/CloudStorage`, system-driven materialisation, `evictItem`, LRU eviction under disk pressure, native Finder decorations. **Kernel extensions are deprecated** — this is the only supported path. |
| **Linux** | **FUSE** | The daemon implements the whole filesystem and fetches on read. But there is **no standard placeholder API, no standard pinning bit, and no cross-desktop badge protocol.** As of 2026 there is still no Linux equivalent to CFAPI or File Provider. |

Two consequences liquid must plan for: macOS's File Provider migration cost real
capability (Dropbox lost LAN sync on macOS under File Provider, and external-drive
support is limited), and **Linux requires designing caching, offline policy, error
surfaces and desktop integration from scratch**.

## The correctness problems that actually bite

These are where sync engines lose data, and they are unglamorous:

- **Filesystem events are hints, not truth.** inotify queues overflow, FSEvents
  coalesces to directory granularity, mounts vanish, the client goes offline. A
  correct engine persists a cursor and **rescans against its database** after
  overflow or restart. Treating the event stream as authoritative is the classic bug.
- **Rename detection needs stable identity.** A stateless scanner sees delete +
  create. Without stable IDs, heuristic matching confuses copies with moves. This is
  precisely why Nucleus introduced globally unique identities.
- **Unicode, case, and path limits.** Platforms disagree on case folding,
  normalisation forms and trailing whitespace. `Records` and `records` cannot coexist
  on a case-insensitive target. Dropbox documents a sub-260-character path limit and
  publishes a bug where Java and Python lowercased differently. Two names that
  collapse locally must produce an **error or a deterministic rename** — never a
  silent overwrite.
- **Files still being written.** Applications save via repeated writes, lock files,
  temp names, or atomic rename. Debounce, check size/mtime stability, and hash a
  consistent snapshot.
- **Atomic publication ≠ durability.** Write to a temp file on the same filesystem,
  verify hashes, rename over the destination — and `fsync` both file and parent
  directory when durability is actually required.
- **Delete versus edit is a real conflict**, not automatically a delete.
- **Authorisation must be checked at commit**, not at open. A device may hold cached
  plaintext after its access was revoked; cached copies cannot be recalled.

## Conflicts: the honest answer is a conflicted copy

Arbitrary binary content has no safe generic merge. Every serious system — Dropbox,
Nextcloud, Syncthing, Resilio — keeps one canonical path and writes the other
version as a conspicuously named conflicted copy.

The usability failure is well documented: the conflicted copy looks like an unwanted
duplicate, so users delete it without comparing, and cannot tell which is
authoritative.

Syncthing's version vectors are the principled decentralised alternative: if
vector A dominates B, A includes B's history; incomparable vectors *prove* concurrent
edits rather than guessing from wall-clock times. But vectors establish causality
only — they still do not merge bytes.

## Sharing: where non-technical users actually fail

The research is blunt: non-technical users have a "muddy" model of access rights,
rarely inspect effective permissions, and cannot predict how move, delete, copy and
email-link operations affect collaborators. Studies identify the "permission clash"
where a link is emailed without granting recipients access.

Specific confusions worth designing against: a share link is a **continuing
capability**, not an attachment; "editor" silently includes deleting and resharing;
permissions are usually inherited from **location**, so moving a file changes its
audience; selective sync hides a folder locally but does **not** revoke anyone's
access; and a downloaded copy cannot be recalled by revoking the share.

The recommended fix is a UI principle: show **who can access, what they can do,
whether access is inherited, and whether the recipient gets a live object, a link,
or an independent copy** — at the moment of sharing, not behind a settings dialog.

## End-to-end encryption costs features

E2EE prevents the server reading content but disables or complicates dedup,
previews, indexing, malware scanning, server-side conversion, web editing and
recovery. Dropbox's encrypted team folders exclude Dropbox from folder keys and
consequently lose shared links, previews, search, API access and several governance
functions. Syncthing's untrusted devices reduce dedup *by design*, because the
untrusted node must not learn equality relationships.

This is the same tension as [14-secrets-and-encryption](../14-secrets-and-encryption/),
and the same resolution applies: dedup in the trusted client with keyed chunk IDs,
before encryption.

## Implications for liquid

1. **Split metadata from blocks, exactly as Dropbox does.** liquid's chunk store is
   the block service. The metadata layer — namespace, paths, revisions, permissions —
   is a separate service with its own ordering. This split is what makes the whole
   product tractable, and liquid already has one half of it.

2. **Use the CRDT datastore for the metadata layer.** The namespace is a
   concurrently-edited structured document, which is exactly what
   [`automerge-cpp`](../../features/17-implementation.md) is for. Its sync protocol
   becomes liquid's replication protocol; its stable object identities solve the
   rename problem that forced Dropbox's rewrite; its time travel gives version
   history for free.

3. **Content-defined chunking beats Dropbox's fixed 4 MiB blocks.** An insertion near
   the start of a file shifts every subsequent fixed block. liquid's FastCDC chunking
   ([02](../02-large-files-and-binary-assets/)) resynchronises after a chunk or two —
   a strictly better delta-sync story than the incumbent.

4. **Implement all three OS integrations properly, and budget for Linux.** CFAPI on
   Windows, File Provider on macOS, FUSE plus bespoke desktop integration on Linux.
   This is the same virtual filesystem serving developer checkouts
   ([16](../16-monorepo-scale-and-authoring-loop/)) — one component, two products.

5. **Treat filesystem events as hints; always be able to rescan.** Persist a cursor,
   reconcile against the database, and handle overflow explicitly.

6. **Adopt the three-tree reconciliation model and deterministic simulation.**
   Nucleus's real lesson is testability: run the reconciliation state machine
   deterministically so a seeded simulator can explore the state space. For a system
   holding people's files, this matters more than throughput.

7. **Conflicts are values, and the UI must say so.** liquid already treats conflicts
   as first-class ([01](../01-version-control/)). For consumers, present a conflict
   as "two versions of this file — here is what differs" using liquid's structural
   diff ([03](../03-structural-diff-and-formats/)), rather than a mysteriously named
   duplicate file.

8. **Normalise names centrally and refuse ambiguity loudly.** Case, Unicode form and
   path length must be checked against both the canonical rules and the destination
   filesystem before publication.

9. **Version history is free, and it is the feature consumers actually want.**
   liquid stores every version as chunks and every change in the CRDT history. "Show
   me last Tuesday's version" is a query, not a backup-restore operation.
