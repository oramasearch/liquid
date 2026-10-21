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
preserved. Every entry name is checked against the tree entry name rule in
[`object-model.md`](object-model.md#records-in-the-store) before it is written, and every file and
directory is created exclusively without following a symbolic link: a name that collides with an
earlier one on the target filesystem, by case or Unicode normalisation, fails with
`errc::already_exists` (liquid #386).

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
cmake -S . -B build -G Ninja && cmake --build build -j 6 && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared -j 6 && ctest --test-dir build-shared --output-on-failure
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

## Backend conformance (#301)

`liquid-vfs-conformance` runs the same nine filesystem cases against an explicitly named driver.
`direct` is a **test backend for calibration**, with no kernel mount. It implements the same
`vfs_detail::backend` lifecycle and counters interface as `fuse_fs.cpp`, and dispatches operations
through the real request queue to repository entry lookup and mutations, the persistent hydration
cache, drive settings and xattr helpers. It emits the common observation records. Its repository
adapter is also used by full materialisation; it does not materialise a checkout to simulate reads.
Synthetic attributes, the recorded-tree overlay and descriptor bookkeeping live in the test driver.
Full materialisation retains its separate portable tests.

`libfuse` still calls the existing mount and observed-mount entry points through POSIX operations.
`fuse_fs.cpp` and `observation.cpp` are compiled unchanged into the test executable so private
entry points remain testable with both static and shared libliquid. The shared target also compiles
private hydration, queue, drive-settings, xattr and CBOR helpers; library visibility stays unchanged.

Under [decision 41](https://github.com/oramasearch/liquid-bridge/issues/28), passing this suite means
**"agrees with liquid's own core"**, which is weaker than **"agrees with a known-good filesystem"**.
The **libfuse-on-Linux baseline is still owed against #299**. Direct calibration does not cancel
that debt, replace a production backend, or allow a real backend to skip these cases.

```sh
cmake -S . -B build -G Ninja
cmake --build build -j 6
ctest --test-dir build --output-on-failure -R '^vfs-conformance-'
build/tests/liquid-vfs-conformance --backend=direct
ctest --test-dir build --output-on-failure

cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared -j 6
ctest --test-dir build-shared --output-on-failure -R '^vfs-conformance-'
ctest --test-dir build-shared --output-on-failure
```

The executable requires `--backend=<registered name>` and rejects unknown names with exit 2.
An invocation requires its selected backend to start. `direct` runs on macOS and Linux without
FUSE or mount privileges. Invoking `libfuse` without a working mount fails with the backend name
and a `mount: expected ...` diagnosis; it never falls back to `direct`. When libfuse
was not found at configure time, CTest adds `--allow-unavailable`; unsupported mounts then return
77 and CTest marks the suite skipped. A skipped case never establishes a conformance baseline.
Linux builds require libfuse3 and therefore require mounted execution by default.

The cases call the shared test operation interface, then independently inspect repository bytes,
operation history, signed drips, observation events, and backend counters. The POSIX adapter calls
the mounted filesystem for every operation; the direct adapter calls the core. The expectations
and fixtures are identical:

| Operations | Expectations |
|---|---|
| `getattr`, `readdir`, `open`, `read` | Recorded input-tree bytes, size, file mode, nested names, EOF and zero-length reads; divergent live repository and physical checkout bytes stay outside the recorded input view. Observations retain the requested blob identity and caller PID. |
| `create`, `write`, close/reopen (`release`) | Offset overwrite and extending writes retain exact bytes with zero-filled gaps; close and remount retain captured content. History carries the TAAP author and stable node, signatures verify, and write events identify the calling process. |
| `truncate`, `chmod`, `open(O_TRUNC)` | Shrink, zero-filled extension, fresh size, preserved file type and permission bits, persisted empty content after truncating open. |
| `rename`, `unlink` | One move to an absent destination retains stable identity; replacement preserves source bytes; lookup, directory names, and repository state agree after removal. |
| `getattr` modification times | Source epoch `1000000000`, declared-output epoch `2000000000`, increasing output mutation times, and stable source time. |
| Read-only observation and mutable capture | Write opens, creation, truncate, chmod, rename and unlink refuse with `EROFS` and record no captured writes. The reserved `deps/` tree remains immutable inside mutable mounts. |
| `getxattr`, `listxattr`, `setxattr`, `removexattr` | Size queries, NUL-separated names, pin/state transitions, reset persistence, short-buffer `ERANGE`, invalid-value `EINVAL`, and unknown-name refusal. |
| Range hydration | A remote blob starts with one local logical chunk. Concurrent reads of two distant missing ranges fetch bytes through the serialized queue, leave some content unhydrated, and reuse the persistent cache after unmount/remount with the remote offline. |

The libfuse backend does not register a `release` callback today. The release contract here is the
observable close/reopen and remount behavior. The suite is scoped to the implemented file and
observation surface; it is not a complete POSIX certification. It does not cover crash atomicity,
open handles across rename/unlink, hard links, symlink traversal, locking, mmap, durability barriers,
all extended-attribute flag combinations, directory mutation, kernel cache coherence, or kernel
request cancellation. The direct driver implements only the operation surface exercised here;
its descriptor and overlay bookkeeping are not a production filesystem contract. Existing VFS
tests retain their materialisation, drive policy, locking, and directory coverage. Windows requires
a native operation adapter in addition to its future mount driver; its current test reports
unavailable explicitly. On Windows CTest permits that skip; the nine-case direct baseline is
macOS/Linux coverage, not Windows coverage.

### Adding a backend

1. Add a `backend_driver` entry in `tests/vfs/conformance_backend.cpp` with a distinct stable name.
   Its `start(mount_request)` must consume the repository, mountpoint, optional recorded input tree,
   declared outputs, observation sink and read-only flag. Return a `backend_instance` with a
   lifecycle handle implementing unmount and counters, and a `filesystem_operations` adapter. Kernel mounts on POSIX can reuse `posix_operations()`; adapt
   other native operations without changing the assertions. Adapt observation events to the common
   record, including caller PID, blob identity and denied writes.
2. Add its implementation and dependencies to the `liquid-vfs-conformance` target. Keep filesystem
   operations and assertions in `tests/vfs/test_conformance.cpp` independent of the selected driver.
   Where the operating system uses different calls, adapt those calls while preserving expectations.
3. Configure `-DLIQUID_VFS_CONFORMANCE_BACKENDS="direct;libfuse;new-name"` to register one CTest per name,
   or invoke `liquid-vfs-conformance --backend=new-name` directly. Unknown names must fail; never
   substitute another driver. Run with an available projection and without `--allow-unavailable`.
4. Record the complete named run and a deliberate regression before accepting a backend. New
   backends belong to #302, #303 and #304 and must pass the suite themselves. `direct` is only
   the calibration reference; it cannot stand in for their runs.

### Calibration and regression demonstration

The recorded baseline is the nine-case `direct` run on native macOS, with no skipped cases.
Static and shared verification, the actual deliberate-failure output, and suite summaries are
recorded in [the proof record](vfs-conformance-proof.md). The libfuse entry remains registered;
on this host its cases skip with an explicit macFUSE diagnosis.

To demonstrate sensitivity, temporarily change the `st_size` assignment in `attributes` in
`tests/vfs/conformance_direct.cpp` from `file->size` to `file->size + 1`, rebuild the target with
`cmake --build build --target liquid-vfs-conformance -j 6`, and run:

```sh
build/tests/liquid-vfs-conformance --backend=direct \
  --gtest_filter=VfsConformance.getattr_readdir_open_read_use_recorded_input_tree
```

The recorded failure reports `attributes.st_size` as `16`, expected `15`, followed by
`getattr: expected st_size to equal recorded input bytes` and `backend=direct`. The mutation was
reverted and all nine cases passed again. Do not commit a mutation or change an expectation to
accommodate a failure. Do not modify the libfuse implementation for this calibration exercise.

**Linux proof deferred.** No Linux lane or macFUSE installation was attempted for this work order.
When a usable Linux libfuse3 mount is available under #299, record the actual mounted run:

```sh
ctest --test-dir build --output-on-failure -V -R '^vfs-conformance-libfuse$'
```

That independent mounted baseline remains outstanding even though direct calibration now runs.

## How to change

Public values and functions belong in `include/liquid/vfs.hpp`; no platform type may enter that
header. Repository traversal and range access stay in the private adapter. Cache policy belongs in
`src/vfs/hydration.cpp`, and callbacks belong in `src/vfs/fuse_fs.cpp`. Add a mount operation only
by giving it a queued runtime request and a repository mutation; a callback must not access the
store or repository directly.
