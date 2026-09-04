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

Remote hydration from lake is also Phase 6. Phase 1 proves that opening a local repository writes
no working-tree bytes and that range reads touch only covering local-store chunks.

## Public API

`mount_options::read_only` rejects modifying operations with `EROFS`.
`mount_options::attr_timeout` controls FUSE attribute and entry caching. The opaque `mount` handle
exposes asynchronous `unmount()` and synchronous `status()`:

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

Plain BLAKE3 and SHA-256 cache entries are rehashed before use. Keyed-digest chunks are decrypted
and authenticated by the store for each read but are not persisted in the plaintext cache, because
the VFS layer does not receive the trust-domain MAC key needed to reauthenticate a cached copy.

The cache never manufactures data. A missing, corrupt, unavailable, or undecryptable chunk maps to
`EIO` on a read and writes its liquid error to standard error. The caller never receives zeroes,
an empty substitute, or ciphertext.

## Request queue and writes

FUSE callbacks copy request parameters into a private request record, enqueue it, and wait on that
record's completion condition. One runtime coroutine drains the queue and is the only code that
touches repository state. Blocking cache and mount-loop work goes through `runtime::offload`, so
callback waits do not occupy the repository executor.

The local queue measurement on macOS 14.6.1 arm64 was:

```text
request_queue_average_latency_ns=38061
```

That is the mean for 100 serialized no-op handoffs from `liquid-vfs-queue-tests`; it measures the
queue and runtime dispatch cost, not storage or kernel FUSE work.

Create and write call `repository::write`, rename calls `repository::move`, and unlink or rmdir
calls `repository::remove`. Directory creation uses the same repository document and journal
path. Advisory-lock failures become `EACCES`. The repository journal records the operation's
stable `node_id`, so a VFS write is visible as a drip without an OS watcher; one rename request is
one recorded move.

## How to verify

Run both library forms:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
```

`VfsMaterialise` compares full and prefix checkouts. `VfsHydration` checks a range read against the
source and asserts that less than the file was hydrated. `VfsMount` uses a 64 MiB file, reads 4 KiB
at 40 MiB, reads the full file, captures a write and stable node identity, records one move, checks
locked-path `EACCES`, and verifies reuse after unmount and remount. It reports a skip with the FUSE
diagnosis when the backend or device is unavailable. Run `liquid-vfs-queue-tests` directly to print
the request-queue measurement.

## How to change

Public values and functions belong in `include/liquid/vfs.hpp`; no platform type may enter that
header. Repository traversal and range access stay in the private adapter. Cache policy belongs in
`src/vfs/hydration.cpp`, and callbacks belong in `src/vfs/fuse_fs.cpp`. Add a mount operation only
by giving it a queued runtime request and a repository mutation; a callback must not access the
store or repository directly.
