# Continuous integration

GitHub Actions runs [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) for every push
and pull request. Checkout recursively initializes the public Capy plus the private Corosio and
automerge-cpp submodules using `ORAMA_SUBMODULES_TOKEN`. The repository secret must exist and
grant read access to `oramasearch/corosio` and `oramasearch/automerge-cpp`; checkout fails before
the build when it is absent. Local clones should use `git clone --recurse-submodules`. Each job
then builds without a dependency cache, runs CTest with failure output, and runs `liquid version`
on each desktop platform.

| Job | Runner | Compiler and build tools | Verification |
|---|---|---|---|
| Linux | `ubuntu-24.04` | Clang 19 from apt.llvm.org, `libstdc++-14-dev`, libfuse3, CMake 3.28 or newer, Ninja | Configure, build, CTest including the FUSE mount scenario, `liquid version` |
| macOS | `macos-15` | Homebrew LLVM, CMake 3.28 or newer, Ninja | Configure, build, CTest, `liquid version` |
| Windows | `windows-2022` | The runner's current `clang-cl` with the MSVC STL, current Chocolatey CMake and Ninja | Configure, build, CTest, `liquid version` |
| Linux sanitizers | `ubuntu-24.04` | Clang 19 with AddressSanitizer and UndefinedBehaviorSanitizer, libfuse3 | Debug configure, build, CTest including the FUSE mount scenario |

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
`-fsanitize=address,undefined` and runs CTest; a sanitizer finding fails the workflow.

The Linux jobs install both `libfuse3-dev` and `fuse3`. Configuration requires the `fuse3`
pkg-config package on Linux, and the mount test exercises a 64 MiB repository through libfuse3
when the runner exposes `/dev/fuse`. If the runner cannot provide a FUSE device, the test reports
that FUSE is unavailable and is skipped rather than silently substituting the materialisation
path. The private submodule token remains an earlier gate: without `ORAMA_SUBMODULES_TOKEN`, CI
stops at checkout before any FUSE proof can run.

## Local Linux verification

Hosted CI cannot build while the org submodule token is missing, and macOS builds hide two
classes of Linux difference: libc++ supplies some standard headers transitively where libstdc++
does not, and `std::int64_t` is `long` on Linux but `long long` on macOS, so a `long long` from
`std::stoll` matches no `std::int64_t` overload exactly. `tools/linux-ci/Dockerfile` is the same
toolchain the Linux jobs use (Ubuntu 24.04, Clang 19, CMake 3.31, Ninja, libfuse3). Build it once
and run the whole suite against a worktree; `--privileged` with `/dev/fuse` lets the mount and
sandbox tests run for real:

```sh
docker build -t liquid-linux-ci tools/linux-ci
docker run --rm --privileged --device /dev/fuse -v "$PWD":/src -w /src liquid-linux-ci \
  bash -c 'cmake -S . -B build-linux -G Ninja && cmake --build build-linux -- -k 0 \
           && ctest --test-dir build-linux --output-on-failure --timeout 600 \
                    -LE "large|sim-long|realworld"'
```

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
allocated loop configuration, and issues one offloaded root lookup after `fuse_exit` during an
explicit unmount. That lookup wakes the blocked libfuse worker without blocking the runtime
executor. A successful named run reports one passed test and no skipped tests.

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
