# Drive synchronization

**What.** Issues #195, #235, and #258's folder synchronization client: persisted three-tree
**What.** Issues #195, #235, and #247's folder synchronization client and clone entry point:
persisted three-tree
reconciliation, per-path device state, lazy and pinned hydration, offline write queues, content
conflicts that retain both values, resumable transfer, path history and restore, daemon-hosted
watching, and Linux file-manager badges.

**Status: implemented by liquid #195, #235, and #258.** `liquid::drive_client` provides one-shot
**Status: implemented by liquid #195, #235, and #247.** `liquid::drive_client` provides one-shot
synchronization, path state, policy configuration, pinning, and status. `liquid drive sync`
exposes synchronization directly; `pin`, `unpin`, and `status` use the same repository settings.
`liquid clone` selects a remote directly or through a configured drive folder and creates a fully
materialized plain checkout through the same client.
`--watch` attaches to the checkout daemon, whose drive target runs the same client after captured
filesystem drips. Lake's web drive is separate consumer work.

## Device states and settings

Every path resolves to exactly one device state. Rules are stored under the local repository's
`drive.settings.v1` metadata key and therefore survive process and machine restarts without a
side database.

| State | Stored value | Behavior |
|---|---|---|
| Online-only | `online-only` | Visible in the FUSE namespace; the first read hydrates verified chunks from the configured sync peer. |
| Available offline | `available-offline` | Pinned content is fully hydrated and remains local across mounts. |
| Not synced here | `not-synced-here` | Explicitly excluded from directory listings, filesystem scans, and local publication. The remote namespace is unchanged. |

The deepest explicit rule wins. With `pin_folders=true`, an available-offline directory rule pins
all descendants; an explicit child rule can return one path to online-only. Exclusions always
apply recursively so disabling folder pins cannot expose an excluded child. The persisted
`hydration_size_threshold` defaults to 8 MiB. An online-only file at or below it hydrates in full
on first read; a larger file hydrates only the chunks covering the read. Pinning always hydrates
the complete file a chunk at a time and does not allocate the complete file in memory.

`drive_status::paths` reports the effective state, size, hydration record, byte debt, and reason
for each file and each explicit rule, including excluded or empty directories. `pending_uploads`
counts paths whose current tree differs from the last accepted base tree. `hydration_debt` sums
non-local bytes in every visible path, including online-only content that has not yet been read.
`offline_reason` retains the latest peer failure until a later hydration succeeds.

## Extended attributes

The Linux FUSE mount provides two virtual user attributes on files and directories:

| Attribute | Read | Write |
|---|---|---|
| `user.liquid.state` | One of the three stored values above. | Sets an explicit per-path rule. |
| `user.liquid.pin` | `1` only for available-offline; otherwise `0`. | `1` pins and hydrates; `0` writes an online-only override. |

`listxattr` returns both names. Removing either attribute writes the online-only state. Unknown
attributes return `ENODATA`, invalid values return `EINVAL`, and an excluded entry returns
`ENOENT` after the kernel's cached entry expires. The Nautilus and Dolphin providers under
[`tools/desktop/linux/`](../../tools/desktop/linux/) read only `user.liquid.state`; they cannot
mutate repository content.

## Repository and three trees

A synchronized folder is an ordinary liquid repository. Its checkpoint, journal, chunks, changes,
drive configuration, and synchronization state all use that repository and its store. The remote
view is a repository with an independent checkpoint namespace over the same store; there is no
side database. The bearer token is never persisted.

The drive state stores these tree-record digests under repository metadata:

| Tree | Meaning | Advances when |
|---|---|---|
| local | The most recently observed and reconciled filesystem namespace. | A full walk is staged and the result is recorded. |
| remote | The most recently pulled server namespace. | A pull imports and verifies the server state. |
| base | The last namespace accepted on both sides. | Push completes with no refused reference. |

State also retains the opaque pull cursor, interrupted-transfer flag, latest per-reference
outcomes, walk count, file count, and estimated resident scan bytes. A checkpoint is written
before network transfer with `interrupted=true`; success or a completed push with refusals writes
it again with `interrupted=false`. A network or process interruption therefore leaves an explicit
resume marker.

Stable `node_id` values identify paths across moves. A full walk reuses the node at the same path;
for a new path it reuses a uniquely removed node with identical content. All maps and reports are
path-sorted, so equal inputs produce equal results.

On the first sync, the client stages filesystem bytes without adding namespace entries, pulls the
existing remote into the still-empty local repository so it can adopt that repository identity,
and initializes the remote view. On later syncs, the local repository merges the pulled remote
directory documents before reconciliation, making its resulting writes causally later than the
server's changes while the independent remote view remains the three-way input. A reconciliation
change names both local and remote heads when they differ. The client then reconciles staged local
files with the pulled tree. This also handles an empty remote, which adopts the local identity on
the subsequent push.

Every imported directory document that can become active is bound to the destination repository's
path-derived actor. The transport receiver identity is never retained for later local writes. Two
fresh clients can therefore edit the same imported directory offline without reusing one actor's
sequence numbers; their later sync retains both named values and records both paths in history.

## Reconciliation table

`B`, `L`, and `R` mean the base, local, and remote value for one stable node. Changed includes
content, mode, or path. Publication always records a normal repository operation and never edits an
older change.

| Base/local/remote observation | Classification | Result |
|---|---|---|
| absent / present / absent | local-only | Keep local, record it, and push. |
| absent / absent / present | remote-only | Keep remote and publish it locally. |
| absent / equal content at the same path / equal content at the same path | converged add | Keep one value without a conflict. |
| absent / different value / different value at the same path | content conflict | Keep local at its name, publish remote at a deterministic conflict name, and record both references. |
| present / unchanged / unchanged | unchanged | Keep the base value; emit no path outcome. |
| present / changed / unchanged | local-only | Keep local and push. |
| present / unchanged / changed | remote-only | Keep remote and publish locally. |
| present / same change / same change | converged change | Keep the common value without a conflict. |
| present / different content changes / different content changes | content conflict | Retain base, local, and remote references; keep both current byte values under visible names. |
| present / moved / unchanged | move | Keep the local path and stable node identity. |
| present / unchanged / moved | move | Publish the remote path with the stable node identity. |
| present / moved only / edited only | move plus edit | Use the moved path and edited bytes. |
| present / edited only / moved only | move plus edit | Use the moved path and edited bytes. |
| present / incompatible changes including two moves | content conflict | Keep both current values and report the original path's conflict. |
| present / deleted / unchanged | local-only delete | Remove locally and push the deletion. |
| present / unchanged / deleted | remote-only delete | Remove the filesystem path. |
| present / deleted / deleted | converged delete | Keep the path absent. |
| present / deleted / edited | delete-versus-edit | Keep the edited remote value and record base plus edited references as a conflict. |
| present / edited / deleted | delete-versus-edit | Keep the edited local value and record base plus edited references as a conflict. |
| prior transfer marker is set | interrupted transfer | Report the interruption, re-pull from the last acknowledged cursor, reconcile, and retry missing chunks and metadata. |
| filesystem event loss or overflow | full-walk recovery | Ignore the incomplete hint set, walk the folder, and reconcile the resulting local tree. |

The current watcher treats events as wakeups. It does not use event order as repository truth. Each
delivery invokes a full walk, and ordinary one-shot sync also walks, so coalescing and queue
overflow converge through the same deterministic query.

## Conflict values and resolution

A concurrent content edit keeps the local bytes at the original path and writes the remote bytes
as `<stem> (remote conflict)<extension>`. If that path exists, numbered suffixes are tried in
ascending order. The repository conflict value names the original path and retains the base when
present plus both current blob-record references. Conflict values are encoded in the mutable
repository checkpoint and in the next change, so reopening the repository cannot discard them.

Both filenames remain ordinary tree entries and can be read, moved, or compared. Resolution is an
ordinary write or remove followed by a cut; repository mutation clears the conflict for that path
and appends new history. No resolution overwrites or rewrites the conflicting change.
An unrelated synchronization retains the conflict while its current references and original path
remain in the tree; rebuilding status does not itself count as resolution.

## Transfer and publication

Pull imports signed repository and tree metadata into the remote view and registers content chunks
against a retained `sync_endpoint`; payload bytes remain remote until policy or a read needs them.
Reconciliation then applies repository operations. Pinned files and online-only files at or below
the threshold are published to the synchronized folder. Larger online-only paths remain virtual
placeholders in the FUSE namespace, and later full walks retain their repository entries while
chunks remain remote. A published file is streamed a chunk at a time to a temporary file beside
its destination, permissions are applied, and the file is renamed into place. Durable mode flushes
the file and parent directory on POSIX systems. Filesystem calls that may block run through
`runtime::offload`.

Push uses the existing sync endpoint after local reconciliation. It enumerates payload identities
without reading them and hydrates only payloads the peer reports missing, so unchanged online-only
files stay remote. Missing-object negotiation makes chunk retries idempotent. The report combines
received object and byte counts from pull and push
and exposes every pushed reference as applied or refused with the server's reason unchanged. A
refusal is a completed transfer, but the base tree does not advance, so the local change remains
pending for a later retry. The CLI prints every outcome and exits with status 1 when any ref is
refused. In watch mode, the drive target records the refused reference and reason as its daemon
target error and leaves its delivery cursor pending for retry.

## Clone entry point

`liquid clone <url|drive-folder> [<dir>] [--json]` is the plain-checkout entry point. A URL becomes
the client's persisted remote; a drive-folder operand reuses that repository's configured remote.
An omitted destination uses the last URL path component or `repository` when the endpoint has no
path component. The command refuses an existing non-empty target before opening the endpoint.

Clone opens the target with the remote-selecting `drive_client::open` overload, pins the empty root,
and runs one synchronization. First-sync identity adoption brings in the remote history. The root
pin makes every pulled descendant available offline, including files above the ordinary hydration
threshold, so the result is a complete working tree rather than a lazy drive view. The stored
remote supports later `drive status` and synchronization. `LIQUID_TOKEN` is supplied only through
the in-memory drive options and is never persisted.

Text output reports the remote and absolute target. JSON reports the path, remote, received object
count, and received byte count. Transport errors retain their public `errc` names; Lake
authentication is `unauthorized`, an absent repository is `not_found`, and a non-empty target is
`already_exists`. A failed clone removes state it created and restores an existing empty target.

## History and restore

`repository::history(path)` walks changes newest first. It finds the path's `node_id` at the first
matching snapshot and follows that identity through earlier names, returning the change, path,
value reference and file mode when present, actor, timestamp, message, and deletion flag. A deletion remains a
history entry even though it has no value.

`repository::restore(path, change)` reads that historical snapshot and records its blob, node
identity, and file mode as a new write at `path`. It does not move a ref or modify a prior change. The caller may inspect or edit the
restored working value before cutting it. `liquid history <path>` lists the entries and
`liquid history <path> --restore <change>` performs the new write.

## Runtime and bounds

All library waiting operations are Capy tasks. The CLI uses `runtime::block_on` only at its command
entry points; the daemon awaits drive tasks on its existing runtime and receives the inherited
bearer token as an in-memory option. FUSE requests, pin hydration, sync reads, and
desktop-independent state changes stay on Capy tasks. CLI waits use Capy timers. Filesystem walks,
directory creation, temporary output, rename, removal, durability flushes, and the FUSE loop use
`runtime::offload`. No drive or VFS component creates a raw thread.

The default resident walk-state ceiling is 256 MiB. The walker accounts each retained path and
record before staging content and returns `errc::busy` when the configured ceiling would be
crossed. File bytes are streamed into the store and do not count as resident scan state. Status
reports both measured state bytes and the selected ceiling. The synthetic-folder regression uses
4,096 files, a 256 KiB walk-state ceiling, and a 1 GiB absolute peak-RSS ceiling for the complete
test process on macOS and Linux.

## How to verify

The deterministic test uses seed 195 and a real HTTP adapter around `sync_server`. It covers every
row that changes state: local-only, remote-only, concurrent edit with both named files, stable-node
move, delete-versus-edit, interrupted chunk download and resume, and event-loss recovery by a later
full walk. It pushes a folder and pulls the result with an ordinary `sync_endpoint`, verifies
conflicts after checkpoint reload, joins a pre-existing non-empty remote, preserves an
authorization refusal reason with an unadvanced base, and rejects the bounded synthetic folder
described above. A separate two-client regression syncs a common file, edits it differently while
both clients are offline, and verifies both names, conflict references, and path histories after
the clients reconnect in sequence. Repository and CLI tests prove
history follows a move and restore appends a drip without changing the old log.
described above. Repository and CLI tests prove
history follows a move and restore appends a drip without changing the old log. The clone scenario
pulls from an HTTP endpoint, fully hydrates a file above the default threshold, checks persisted
remote and JSON output, and builds a small CMake project inside the checkout. Refusal cases pin the
authentication, unknown-repository, and non-empty-target error names.

Portable VFS tests cover the three-state resolver, recursive folder pins, explicit child
overrides, xattr names and values, and invalid pin values. Drive tests persist settings, retain a
large online-only placeholder across repeated sync, surface an offline pin reason, pin a folder,
record hydration, override one child, reopen the client, queue a repository write, reconcile it
later, and verify status counters. Linux FUSE coverage reads and writes the real attributes.
Python string tests pin the Nautilus and Dolphin badge mappings.

## Deferred consumers

Windows CFAPI, macOS File Provider, distro packaging, and lake's browser, preview, sharing, and
web-drive surfaces remain outside this repository change. They consume the public drive and
repository APIs described here.
