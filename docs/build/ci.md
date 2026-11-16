# Continuous integration

GitHub Actions runs [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) for every push
and pull request. Checkout recursively initializes the public Capy plus the private Corosio,
automerge-cpp, HTTP, and oramacore-cpp submodules using `ORAMA_SUBMODULES_TOKEN` when that secret
is available, otherwise the job token. The preferred repository secret grants read access to
`oramasearch/corosio`, `oramasearch/automerge-cpp`, `oramasearch/http`, and
`oramasearch/oramacore-cpp`; a job token without that access fails during submodule checkout.
Local clones should use
`git clone --recurse-submodules`. Each job
then builds without a dependency cache, runs CTest with failure output, and runs `liquid version`
on each desktop platform.

The `Offline CLI (Linux)` and `Offline CLI (macOS)` jobs build the same tree and run
`ctest --test-dir build --output-on-failure -L liquid-offline`. That test enters liquid's own
platform sandbox with networking denied, checks the four permitted network requirements, and runs
the complete `liquid-cli` label inside it. The label is also part of the ordinary local static and
shared suites.

The separate `Documentation site` job installs the pinned `mkdocs==1.6.1`, runs the same internal
link checker registered as `docs-links`, renders all of `docs/`, and uploads
`liquid-documentation-site` as a workflow artifact. The artifact is the published deliverable;
hosting is intentionally not selected here. CLI reference freshness remains a CTest responsibility,
so each binary build regenerates Markdown and stable JSON from its own typed command table before
the site job packages the committed tree.

| Job | Runner | Compiler and build tools | Verification |
|---|---|---|---|
| Linux | `ubuntu-24.04` | Clang 19 and clang-format 19 from apt.llvm.org, `libstdc++-14-dev`, OpenSSL, zlib, libfuse3, SquashFS tools, PostgreSQL server/client, Autoconf, Automake, GNU `time`, CMake 3.28 or newer, Ninja | Configure, build, CTest including the FUSE mount, SquashFS round trip, and PostgreSQL data-branch scenarios; run the AppImage extraction peer; `liquid version` |
| macOS | `macos-15` | Homebrew LLVM, OpenSSL 3, PostgreSQL 17, Apple installer and signing tools, CMake 3.28 or newer, Ninja | Configure, build, CTest including the PostgreSQL data-branch scenario; pour and test a projected bottle from a temporary tap; byte-identical package projection; temporary-Keychain signing; `pkgutil`, install, launch, and sidecar verification; `liquid version` |
| Windows | `windows-2022` | The runner's current `clang-cl` with the MSVC STL, Chocolatey OpenSSL, Windows SDK `makeappx` and `signtool`, current CMake and Ninja | Configure, build, CTest; MSIX projection; temporary-certificate Authenticode signing; `Add-AppxPackage`, launch, signature and sidecar verification; `liquid version` |
| Linux sanitizers | `ubuntu-24.04` | Clang 19 with AddressSanitizer and UndefinedBehaviorSanitizer, OpenSSL, zlib, libfuse3 | Debug configure, build, CTest including the FUSE mount scenario |
| Linux self-host | `ubuntu-24.04` | Clang 19, CMake, Ninja, libfuse3 | CMake bootstrap, two `liquid wrap` builds, 100% second-run disk hits, byte-identical static library and CLI, shared libliquid build |
| Offline CLI | `ubuntu-24.04`, `macos-15` | The platform toolchain above | Every `liquid-cli` test inside the platform sandbox with networking denied; explicit push, pull, remote-execution, and remote-cache refusals |

Every static or shared CTest run includes `consumer-project-build`. It installs the already-built
parent artifacts into a test-local prefix, configures a separate CMake project against that package
with the parent toolchain, then builds and runs its own `liquid::liquid` client. Only the consumer
source is compiled during the test; liquid and its dependencies are not rebuilt.
`cli-identity-codec` also verifies that the public identity codec reads
the initial and rotation documents written by the CLI. The frontend cases compile the standalone C
ABI header, load the Clang frontend shared object, exercise static registration, and run the C and
C++ unit tests against the same pinned tree-sitter runtime and grammars on every platform.

`CMakeLists.txt` selects Homebrew LLVM on macOS, a versioned `clang++` on Linux, and `clang-cl`
for both C and C++ on Windows before `project()`. GoogleTest enables C when FetchContent adds it;
using `clang-cl` for that language prevents CMake from selecting GNU-style `clang.exe`, which
cannot consume the Visual Studio flags passed by its developer environment. If no compiler is
found, configuration stops with instructions for installing one or choosing an explicit system
compiler. The Windows job updates CMake before configuration because the image's CMake 3.31 does
not recognize `cxx_std_26` for Clang 19.1.5; the current package maps that requirement to
`clang-cl`'s `/std:c++latest`. The workflow relies on this paired automatic selection rather than
overriding only the C++ compiler. It initializes the Visual Studio developer environment so
`clang-cl` uses the MSVC STL and linker.

## Platform coverage

`macos-15` is an Apple Silicon runner, so the matrix includes arm64 coverage through macOS. Linux
is currently x86_64 only: GitHub-hosted arm64 Linux runners are not available to this private
repository at no cost. Linux arm64 verification is deferred until a suitable runner is available.

The sanitizer job is separate from the release-style matrix. It configures a Debug build with
`-fsanitize=address,undefined` and runs CTest; a sanitizer finding fails the workflow. Those flags
reach the C++ sources only. The local configuration `-DLIQUID_SANITIZE=address;undefined`
instruments the C sources and the dependencies too, and is the one to use; no CI runs it
([`hardening.md`](hardening.md)).

The installer steps run after the ordinary suite. macOS projects the same native fixture twice and
compares the unsigned packages byte for byte. It creates distinct self-signed application and
installer identities in a temporary Keychain, trusts the installer certificate only for the job,
signs without notarisation, installs to `CurrentUserHomeDirectory`, runs the installed executable,
and removes the trust entry. Windows discovers the newest installed SDK tools, creates and trusts a
temporary code-signing certificate, projects and signs an MSIX, checks
`Get-AuthenticodeSignature`, installs with `Add-AppxPackage`, runs the installed executable, and
removes the package and certificate. Neither job has release credentials. The first real Apple
notarisation, ticket, date, and `spctl` result remain the explicit release gate in
[projection.md](../library/projection.md#macos-package-and-disk-image).

The hot-update demo builds a loadable definition object and drives one `liquid-hot-demo` process
through `value=1` and `value=2`, proving tier-1 redirection at its Capy resumption boundary on
macOS arm64 and Linux x86_64. The tier-3 case executes the restart hook through the ordinary build
sandbox and verifies the appended outcome, old result, new result, and tier arguments. Linux CI
always has libfuse3 for that test. A macOS machine needs macFUSE; without it the restart-hook case
reports the same explicit skip as the other sandbox-dependent tests. Classification and the live
tier-1 path still execute.

The Linux jobs install `libfuse3-dev`, `fuse3`, Autoconf, and Automake. Configuration requires the `fuse3`
pkg-config package on Linux, and the mount test exercises a 64 MiB repository through libfuse3
when the runner exposes `/dev/fuse`. If the runner cannot provide a FUSE device, the test reports
that FUSE is unavailable and is skipped rather than silently substituting the materialisation
path. A job token must have access to every private submodule when `ORAMA_SUBMODULES_TOKEN` is not
available; otherwise CI stops at checkout before any FUSE proof can run.

No machine reachable from the development Mac currently has a FUSE mount, so the wrapped-build
proofs on this page have not been run end to end. What was checked, and the decision needed, is in
the [Phase 2 exit lane audit](phase-2-exit-lane-audit.md).

## Offline CLI prerequisites and diagnosis (#296)

Run the outer CTest command from a native host shell. The test itself creates the denied-network
environment: macOS uses `sandbox-exec`, allowing local Unix-domain sockets while denying TCP,
including loopback; Linux uses the build wrapper's user, mount, and network namespaces.
The host must permit that sandbox mechanism, subprocess creation, writable fixture and temporary
directories, and local Unix-domain sockets. The CLI starts its own checkout daemon, so no
pre-existing daemon or network service is required. Daemon-backed commands also require the
platform file watcher to work; macOS needs FSEvents to report the capture arming cookie.

The format test's ZIP, HDF5, JSON, and text diff checks read the repository directly. Its final
`search changed` assertion starts the daemon. The daemon opens its listener before opening
capture; if capture startup fails, destroying the daemon state closes that listener. A client
already waiting for a status response can then see `daemon socket closed`. Normal CLI startup
redirects daemon stderr to `/dev/null`; running `liquid daemon --serve <checkout>` in the
foreground exposes the startup error. An FSEvents arming failure is distinct from an IP-socket
permission failure and can also occur outside the offline sandbox.
FSEvents arming reliability is tracked in [#282](https://github.com/oramasearch/liquid/issues/282).

`guides-resolution` had a separate, deterministic fixture error: the guide runner reserved a TCP
loopback port before every shell block, even though package resolution never used it. The macOS
offline sandbox correctly refused that bind with `PermissionError: [Errno 1] Operation not
permitted`. This guide test was added in `e5db43f` (#298), after the original #296 baseline
`7ff6ed8`, so it cannot explain that earlier socket failure. The runner now allocates
`LIQUID_GUIDE_PORT` only when the block references it. The local-peer guide keeps its port allocation,
and every existing offline CLI test and assertion still runs. The nested CTest continues after a
failed guide, so the guide failure does not prevent
`cli-format-diff` from running or explain a daemon socket closure.

## Local Linux verification

Hosted CI cannot build when neither the org submodule token nor the job token can read the private
submodules, and macOS builds hide two
classes of Linux difference: libc++ supplies some standard headers transitively where libstdc++
does not, and `std::int64_t` is `long` on Linux but `long long` on macOS, so a `long long` from
`std::stoll` matches no `std::int64_t` overload exactly. `tools/linux-ci/Dockerfile` is the same
toolchain the Linux jobs use (Ubuntu 24.04, Clang 19, CMake 3.31, Ninja, libfuse3, PostgreSQL,
Autoconf, Automake, and GNU `time` at `/usr/bin/time` for ad-hoc measurements). Build it once
and run the whole suite against a worktree; `--privileged` with `/dev/fuse` lets the mount and
sandbox tests run for real:

```sh
docker build -t liquid-linux-ci tools/linux-ci
docker run --rm --privileged --device /dev/fuse -v "$PWD":/src -w /src liquid-linux-ci \
  bash -c 'pg_version=$(ls /etc/postgresql | sort -Vr | head -1) \
           && pg_ctlcluster "$pg_version" main start \
           && runuser -u postgres -- psql -c "ALTER USER postgres PASSWORD '\''liquid'\''" \
           && export LIQUID_TEST_POSTGRES_URL=postgresql://postgres:liquid@127.0.0.1:5432/postgres \
           && cmake -S . -B build-linux -G Ninja && cmake --build build-linux -- -k 0 \
           && ctest --test-dir build-linux --output-on-failure --timeout 600 \
                    -LE "large|sim-long|realworld"'
```

`DataBranch.isolates_discards_promotes_and_refuses_a_changed_base` starts a private cluster on a
reserved loopback port when the test process is not root. The root-run Linux image starts its
packaged cluster explicitly and supplies `LIQUID_TEST_POSTGRES_URL` as above. The test reports its
missing server/client prerequisite when neither route exists; Windows records the documented
unsupported skip. `DataBranch.automatic_mechanism_checks_reflink_support_and_cluster_eligibility`
runs on every platform and exercises the filesystem probe without requiring PostgreSQL.

The included `consumer-project-build` is the installed-package proof; run it alone with:

```sh
ctest --test-dir build-linux --output-on-failure --timeout 120 \
  -R '^consumer-project-build$'
```

The test uses a fresh install prefix and consumer build directory on each run. It copies the
artifacts already produced by the parent build, configures only the small consumer project, and
compiles only that project's source. It remains in the ordinary container suite rather than using
the `large` label.

`-k 0` keeps building past the first failure so one run lists every portability error. Run the
Linux mount scenario by itself, without allowing a missing FUSE device to hide it, with:

```sh
docker run --rm --memory 2g --privileged --device /dev/fuse \
  -v "$PWD":/src -w /src liquid-linux-ci \
  build-linux/tests/liquid-tests \
  --gtest_filter=VfsMount.hydrates_captures_and_reuses_cache
```

The mount originally passed `-f` directly to `fuse_new`, which rejects command-line-only options,
and later asked `fuse_exit` to stop a loop whose worker remained blocked in `/dev/fuse`. The Linux
path now passes `-f` and the mountpoint through `fuse_parse_cmdline`, gives `fuse_loop_mt` an
allocated loop configuration, and issues one offloaded fresh-path lookup after `fuse_exit` during
an explicit unmount. The uncached lookup wakes the blocked libfuse worker without blocking the runtime
executor. A successful named run reports one passed test and no skipped tests.

For a Linux self-host measurement, bind-mount the checkout at `/liquid/work` and set
`LIQUID_WRAP_CACHE=/liquid/work/.liquid-build/self-host-cache`; both the wrapped build tree and
the cache then live on the host bind mount rather than Docker's writable layer. Run
`bash tools/linux-ci/self-host-memory.sh <log> <wrap command>` inside the container to sample
the cgroup-v2 `memory.current` and `memory.stat` fields plus `VmRSS`/`VmHWM` for the wrapper and
its descendants every ten seconds. Keep the log with the self-host result, particularly on a
memory-limit failure.

`BuildWrap.*` runs the CMake, recursive Make, compilation-database, and Autotools fixture coverage.
The local image includes Autoconf and Automake so every Linux fixture executes rather than skipping.
`guides-run` executes the marked shell blocks in `docs/guides/` against the same CMake and Autotools
fixtures, including second-run cache hits, miss explanation, a local sync peer, and the current
identity commands. It returns CTest's skip code only when `/dev/fuse` is unavailable; the privileged
container proof must execute it.

Test processes create a random fixture directory beneath the platform temporary root through
`tests/temporary_directory.hpp`. On Linux this honours `TMPDIR`, which allows container runs to
place fixture data on an explicit writable mount. Fixed fixture names are permitted only beneath
the process directory, so full suites from separate worktrees cannot share test stores or working
directories. The process directory is removed when the test process exits; an unwritable temporary
root stops the test with a diagnostic naming the root.
`FrontendModule.*` and `FrontendGraph.*` exercise dynamic loading, the C fallback, real definition
compiler actions, relocatable-object assembly, and body/header invalidation in the same container.

Run the Linux restart scenario by itself with:

```sh
docker run --rm --memory 2g -v "$PWD":/src -w /src liquid-linux-ci \
  ctest --test-dir build-linux --output-on-failure --timeout 600 \
  -R '^Drip.restart_replays_the_persisted_journal$'
```

The apparent restart growth was not journal replay, reconciliation, stat metadata, or inotify
record parsing. A debugger caught `capture::open` suspended at its first `runtime::offload`, with
the blocking pool idle and no wake queued on the caller. The pool could finish before the caller
registered its `async_event` wait; Capy events are executor-affine and unsafe for concurrent shared
access, so the set/wait race could lose the wake. `offload` now registers the caller continuation
before dispatch and has the pool use `run_async` to schedule a caller-executor coroutine which
queues that continuation. A successful named run reports one passed test.

As of this change `main` compiles on Linux, and both the mount and drip restart scenarios pass
there. To change the toolchain, edit the Dockerfile and the workflow together; they must agree on
the compiler version.

The image's inputs are pinned (lake#1003). `ubuntu:24.04` is named by its multi-platform index
digest (`sha256:2198e4c6…`), and the CMake 3.31.5 installer is checked against the SHA-256 Kitware
publishes for each architecture before it runs. The REAPI generator image (`tests/reapi/Dockerfile`)
pins `python:3.13.7-slim-bookworm` the same way (`sha256:adafcc17…`), beside the commits it already
pins for its proto sources. clang-19 comes from the apt.llvm.org noble channel added directly, not
by running `llvm.sh`: its signing key is trusted only when the key's fingerprint is
`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`, and apt checks every package against it. Not pinned:
the Ubuntu, Debian and LLVM package *versions*, which come from each archive's current state, signed
by its key. To move a pin, read
the new value without building and change it in a reviewed commit:

```sh
docker buildx imagetools inspect ubuntu:24.04 | grep '^Digest:'
curl -sL https://github.com/Kitware/CMake/releases/download/v<version>/cmake-<version>-SHA-256.txt
```

The CLI documentation check extracts the backticked command cell from each Markdown table row.
It must not split table rows into CMake lists: CMake policy CMP0007 differs on whether the leading
empty field before a Markdown row's first `|` is retained.


Test discovery (`gtest_discover_tests`) runs each test binary once at build time with a 120 s timeout (the CMake default is 5 s, which a loaded host exceeds and reports as a build failure).


## When the platform watcher stops delivering

`liquid capture` arms its watcher by writing a cookie file into the workspace and waiting for the
platform to report it. If the platform accepts the subscription and then reports nothing, capture
**does not fail**: it records the condition and runs on periodic reconciliation, which observes the
same changes on `capture_options::reconcile_interval` rather than as they happen.

`capture_status` says which mode it is in:

| Field | Meaning |
|---|---|
| `watcher_armed` | false when the watcher never delivered and capture is reconciling |
| `watcher_diagnostic` | why, and what to do about it — empty when armed |

**On macOS this is usually a wedged `fseventsd`.** It can stop delivering events process-wide while
still accepting subscriptions, so every FSEvents consumer goes quiet at once. It is not liquid, and
it is reproducible without liquid — a forty-line program that creates an `FSEventStream`, writes a
file into the watched directory and waits will report nothing:

```text
RESULT: FSEvents delivered NOTHING
```

The repair is to restart the daemon, which macOS relaunches immediately:

```sh
sudo killall -9 fseventsd
```

Before this was understood it cost four separate sessions, because twelve to fourteen `Drip.*` and
`Daemon.*` tests failed with `FSEvents did not arm for workspace` and looked like a liquid
regression (#282). They now pass on such a host, because capture no longer needs the watcher to
function — only to be prompt.
