# Virtual filesystem and materialisation

**What.** `liquid::vfs::mount` presents a repository without first writing a working tree. Reads
hydrate only the logical chunks that cover the requested byte range. `liquid::vfs::materialise`
writes a conventional checkout for tools and platforms that cannot use a virtual filesystem.

**Why.** Repository metadata and file bytes are already separate. Preserving that split at the
filesystem boundary lets a repository open immediately while disk use follows actual reads. The
full checkout remains a correctness path, not a different repository format.

## Phase 1 platform coverage

| Platform | Lazy mount | Full checkout |
|---|---|---|
| Linux | libfuse3 high-level API; required at build time and found through pkg-config | Supported |
| macOS | macFUSE high-level API; optional at build time | Supported |
| Windows | Not in Phase 1 | Supported |

The 2026-09-04 decision keeps macOS File Provider and Windows Cloud Files out of Phase 1 because
both require a signed host application. They arrive with the drive in Phase 6. FSKit is not part
of this implementation. A macOS build without macFUSE succeeds; attempting to mount returns
`errc::unsupported` with an installation instruction. Windows returns `errc::unsupported` and
directs callers to `materialise`.

Remote hydration through `remote_blob_source` is implemented independently of any hosted service.
Configured drive mounts retain a `sync_endpoint` as that source; the VFS and record format do not
depend on lake.

## Public API

`mount_options::read_only` rejects modifying operations with `EROFS`.
`mount_options::attr_timeout` controls FUSE attribute and entry caching.
`mount_options::bearer_token` is used only while registering a configured drive peer and is never
persisted. The opaque `mount` handle exposes asynchronous `unmount()` and synchronous `status()`:

- `hydrated_bytes`: bytes in logical chunks used by files during this mount;
- `hydrated_files`: files with at least one hydrated chunk;
- `misses_served`: chunks fetched from the repository store rather than the persistent cache;
- `writes_captured`: successful create, write, move, remove, make-directory, and remove-directory
  requests;
- `request_queue_latency`: mean callback-submission-to-completion latency for this mount.

`materialise(repository, target, path_prefix)` requires a missing or empty target. With no prefix,
it reproduces the entire tree. A directory prefix writes that directory's contents into the target;
a file prefix writes that file under its basename. File and directory permission bits are
preserved.

## Hydration and cache

Repository blob records carry each logical FastCDC chunk's digest and size. The sizes let a range
reader locate a 4 KiB request at 40 MiB without fetching earlier chunks. A file moves through three
states: no chunks used, some chunks used, and every chunk used. The cache stores verified plaintext
chunks under `LIQUID_VFS/chunks/` in the repository. It is shared across mounts and capped at 1 GiB;
least-recently-used files are removed when a new chunk crosses the cap.

A version 2 blob may also carry its whole-content digest and remote source/instance. A cache miss
then asks the store for the whole logical chunk at its file offset. The store calls
`remote_blob_source::read_range`, verifies the chunk digest, and stores it as an ordinary local
chunk before the hydration cache publishes it. Concurrent mount requests still pass through the
existing serialized hydration queue. `materialise` reads the whole content digest and therefore
fetches any chunks that remain remote.

Plain BLAKE3 and SHA-256 cache entries are rehashed before use. Keyed-digest chunks are decrypted
and authenticated by the store for each read but are not persisted in the plaintext cache, because
the VFS layer does not receive the trust-domain MAC key needed to reauthenticate a cached copy.

The cache never manufactures data. Corrupt or undecryptable content maps to `EIO`. When an
unhydrated drive path needs a peer that is unavailable, the read returns `EAGAIN`, records
`peer unreachable while hydrating <path>` in drive settings, and leaves a retryable status reason.
The caller never receives zeroes, an empty substitute, or ciphertext.

Drive reads consult the persisted policy before entering the cache. Pinned files and online-only
files no larger than the configured threshold hydrate completely; larger online-only files hydrate
only the requested chunks. A successful full hydration adds a persisted path record; partial reads
remain visible through byte debt and the mount's hydrated-byte counters. FUSE `getxattr`,
`listxattr`, `setxattr`, and `removexattr` expose the state contract in
[`drive.md`](drive.md#extended-attributes) through the same serialized request queue.

## Request queue and writes

FUSE callbacks copy request parameters into a private request record, enqueue it, and wait on that
record's completion condition. One runtime coroutine drains the queue and is the only code that
touches repository state. Blocking cache and mount-loop work goes through `runtime::offload`, so
callback waits do not occupy the repository executor.

On libfuse3, `-f` and the mountpoint are consumed by `fuse_parse_cmdline` before the remaining
filesystem options reach `fuse_new`. The request loop receives an allocated `fuse_loop_config`.
Explicit unmount first records the stop and calls `fuse_exit`, then schedules one root attribute
lookup through `runtime::offload`. That request gives the worker blocked in `/dev/fuse` its next
opportunity to observe the exit flag without blocking a coroutine executor. Cleanup then unmounts
and destroys the session; an unexpected loop result remains a mount error.

The local queue measurement on macOS 14.6.1 arm64 was:

```text
request_queue_average_latency_ns=38061
```

That is the mean for 100 serialized no-op handoffs from `liquid-vfs-queue-tests`; it measures the
queue and runtime dispatch cost, not storage or kernel FUSE work.

Create and write call `repository::write`, rename calls `repository::move`, and unlink or rmdir
calls `repository::remove`. Directory creation uses the same repository document and journal
path. Exclusive-checkout failures become `EACCES`. The repository journal records the operation's
stable `node_id`, so a VFS write is visible as a drip without an OS watcher; one rename request is
one recorded move.

Tree entries always carry Git-style modes: `0100644` for ordinary files, `0100755` for executable
files, `0120000` for symbolic links, and `040000` plus permission bits for directories. Every VFS
writer supplies the complete mode. Create combines the regular-file type with the requested
permissions, while write, truncate, and open with `O_TRUNC` retain the existing entry mode. Chmod
changes only permission bits and retains the current file type, including the symbolic-link type.

## How to verify

Run both library forms:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
```

`VfsMaterialise` compares full and prefix checkouts. `VfsDriveState` and `VfsDriveXattr` cover
policy inheritance and the portable attribute mapping. `RemoteOutputs` proves exact remote chunk
reads, persistent cache reuse, and on-demand full materialisation. `VfsHydration` checks a range read against the
source and asserts that less than the file was hydrated. `VfsMount` uses a 64 MiB file, reads 4 KiB
at 40 MiB, reads the full file, captures a write and stable node identity, records one move, checks
locked-path `EACCES`, and verifies reuse after unmount and remount. On Linux it also exercises the
real drive xattrs and checks `EAGAIN` plus the persisted reason from an unavailable peer. It reports
a skip with the FUSE diagnosis when the backend or device is unavailable. Run
`liquid-vfs-queue-tests` directly to print
the request-queue measurement. On Linux, the command in [`docs/build/ci.md`](../build/ci.md#local-linux-verification)
runs the mount scenario alone and must report one passed test rather than a skip.

## How to change

Public values and functions belong in `include/liquid/vfs.hpp`; no platform type may enter that
header. Repository traversal and range access stay in the private adapter. Cache policy belongs in
`src/vfs/hydration.cpp`, and callbacks belong in `src/vfs/fuse_fs.cpp`. Add a mount operation only
by giving it a queued runtime request and a repository mutation; a callback must not access the
store or repository directly.
