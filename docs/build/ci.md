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
`-fsanitize=address,undefined` and runs CTest; a sanitizer finding fails the workflow.

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

## Phase 2 exit: lane audit for issue #299

**2026-09-08 result: stopped at the lane prerequisite.** No usable existing execution lane
was established from the development Mac. The Phase 2 exit remains undemonstrated. This audit
inspected Liquid revision `e5db43f2dd33a29868d7dcddc93600e47dd03236` on
`issue-299-phase2-exit`, read [#299](https://github.com/oramasearch/liquid/issues/299),
[#291](https://github.com/oramasearch/liquid/issues/291), and the
[Phase 2 epic](https://github.com/oramasearch/liquid/issues/34), and ran only prerequisite
checks. No Liquid code was changed, no demonstration project was built, and no cache or
attestation result is claimed.

[Bridge decision 33](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md#33-proof-lanes--decided-steve-2026-09-05-no-docker-on-steves-machine-for-anything-in-this-project)
forbids Docker on the development Mac. The container commands later on this page do not apply
to that host. Decision 33 describes an off-host AKS lane in namespace `ci`, reached through
`/Users/stevesperandeo/dev/oramasearch/codex-work/tools/linux-proof.sh`. At this audit, the
entire shared `tools` directory was absent and `ci` had no pods. **Linux proof deferred.**

### Does macOS have a path without macFUSE?

**No for fresh wrapped execution or sampled re-execution.**
[`execute_observed_run`](../../src/build/wrap.cpp) calls `start_observed_fuse` before launching
the wrapped driver and propagates its failure. The macOS branch of
[`observation.cpp`](../../src/vfs/observation.cpp), compiled when neither FUSE backend was
found, returns `errc::unsupported` with this diagnostic:

```text
sandboxed actions require macFUSE on macOS; install macFUSE and rebuild liquid
```

That is the source diagnostic, not output from a newly built binary in this audit.
[`CMakeLists.txt`](../../CMakeLists.txt) detects `fuse3` or `fuse`, then checks macFUSE headers
and libraries under `/usr/local` and `/opt/homebrew`. The
[action executor](../../src/build/executor.cpp) also requires the observed mount.

There is a narrower cache-only path: `serve_outer_action` can validate and materialise an
existing complete result before `execute_observed_run`. It requires previously recorded
toolchain/cache state and cannot produce the initial build or run the requested verification
sample. It therefore does not remove the Phase 2 prerequisite. Plain VFS materialisation and
configuring a remote backend do not supply a replacement observed execution backend.

Local checks:

```sh
uname -srm
pkg-config --modversion fuse fuse3
```

```text
Darwin 23.6.0 arm64
Package fuse was not found in the pkg-config search path.
Perhaps you should add the directory containing `fuse.pc'
to the PKG_CONFIG_PATH environment variable
Package 'fuse' not found
Package 'fuse3' not found
```

The `pkg-config` command exited 1. Filesystem checks also found no
`/Library/Filesystems/macfuse.fs`, `/usr/local/include/fuse/fuse.h`,
`/usr/local/lib/libfuse.dylib`, or `/opt/homebrew/lib/libfuse.dylib`.
No `liquid` executable was on `PATH` and this worktree had no build directory. The commands
`limactl`, `multipass`, `virsh`, `qemu-system-aarch64`, `qemu-system-x86_64`, and `utmctl`
were absent from `PATH`; this establishes no installed lane through those commands, not an
inventory of every possible remote machine.

### Off-host Linux checks

The native build host named by the
[org build instructions](https://github.com/oramasearch/orama-bridge/blob/main/.claude/skills/one-orama-dev/SKILL.md)
was unreachable:

```sh
ssh -o BatchMode=yes -o ConnectTimeout=10 -o ConnectionAttempts=1 \
  -o StrictHostKeyChecking=yes -o UpdateHostKeys=no \
  -o ControlMaster=no -o ControlPath=none \
  ssperandeo@192.168.123.175 'uname -srm; hostname'
```

```text
ssh: connect to host 192.168.123.175 port 22: Operation timed out
```

The connection exited 255 before the remote commands could execute. No assertion about that
host's current toolchain or FUSE support follows from a timeout.

AKS context `orama-production-cluster-01` was reachable. Its API reported Kubernetes
`v1.35.7`, platform `linux/amd64`. These reads discover the proof namespace and BuildKit
workers without changing the active context:

```sh
kubectl --context orama-production-cluster-01 --request-timeout=15s \
  get --raw /api/v1/namespaces/ci/pods
kubectl --context orama-production-cluster-01 --request-timeout=15s \
  get --raw /api/v1/namespaces/buildkit/pods
```

The first response had zero items. The second identified the running pod
`buildkit/aks-amd640-b8cc87cfb-rdt7h`, on node `aks-agentpool-33345110-vmss000000`, using
`moby/buildkit:buildx-stable-1-rootless`. There was one BuildKit deployment replica.
Its container arguments were `--allow-insecure-entitlement=network.host` and
`--oci-worker-no-process-sandbox`; its mounts supplied BuildKit storage and the Kubernetes
service-account files, with no FUSE device mount. Its security context declared unconfined
seccomp and AppArmor profiles and did not declare privileged mode.

After rediscovering the current pod name, rerun its prerequisites with the following command.
The local Kubernetes discovery cache stays inside the worktree:

```sh
mkdir -p .liquid-build/issue-299/kube-cache
kubectl --cache-dir="$PWD/.liquid-build/issue-299/kube-cache" \
  --context orama-production-cluster-01 --request-timeout=15s \
  -n buildkit exec aks-amd640-b8cc87cfb-rdt7h -- sh -c '
uname -srm
id
if test -e /dev/fuse; then
    ls -l /dev/fuse
else
    printf "/dev/fuse: absent\n"
fi
for tool in liquid clang clang++ cmake ninja make autoconf automake pkg-config fusermount3; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf "%s: %s\n" "$tool" "$(command -v "$tool")"
    else
        printf "%s: absent\n" "$tool"
    fi
done
'
```

Observed output, exit 0:

```text
Linux 6.8.0-1059-azure x86_64
uid=1000(user) gid=1000(user) groups=1000(user)
/dev/fuse: absent
liquid: absent
clang: absent
clang++: absent
cmake: absent
ninja: absent
make: absent
autoconf: absent
automake: absent
pkg-config: absent
fusermount3: /usr/bin/fusermount3
```

Inside that pod, `buildctl --addr unix:///run/user/1000/buildkit/buildkitd.sock debug workers
--verbose` succeeded. It reported worker `qqd5rbxrid3mjl0zpg5643rty`, BuildKit `v0.32.2`,
an OCI executor, an overlayfs snapshotter, and `no-sandbox` process mode. A working BuildKit
service and an installed `fusermount3` executable do not establish access to `/dev/fuse`.
The existing worker therefore does not provide the observed-mount execution lane. No build
was submitted and no cluster settings, devices, packages, or workloads were changed.

GitHub-hosted Linux also did not provide an available proof in the inspected
[main CI run 34178591912](https://github.com/oramasearch/liquid/actions/runs/34178591912).
This read shows the failure:

```sh
gh run view 34178591912 --repo oramasearch/liquid --log-failed
```

The Linux self-host job's checkout output includes:

```text
remote: Repository not found.
fatal: repository 'https://github.com/oramasearch/automerge-cpp.git/' not found
fatal: repository 'https://github.com/oramasearch/corosio.git/' not found
fatal: repository 'https://github.com/oramasearch/oramacore-cpp.git/' not found
```

These are excerpts from separate checkout failures. They occurred before a Liquid build.
This workflow's existing self-host job performs both wrapped runs on one runner and does
not establish the different-machine clause even after checkout access is repaired.

### Missing evidence and the decision needed

The audit stops here under issue #299's work order. No PR is opened for a stop at the lane
question. The five demonstrations remain unrun:

| Exit evidence | Result of this audit |
|---|---|
| Unmodified real CMake project | Not run: no fresh wrapped-execution lane. |
| Unmodified real Autotools project | Not run: same prerequisite. |
| Cache reuse on a different machine, with hit counts and timings | Not run: no pair of usable compatible build machines was established. |
| Artifact inventory with attestations and a successful verification | Not run: no artifacts were produced. |
| Injected non-determinism caught by sampled re-execution | Not run: no actions were executed or sampled. |

The owner must restore or designate an off-host Linux proof lane with a usable `/dev/fuse`,
Liquid's sandbox permissions, the documented compiler/build prerequisites, and access to the
private submodules; or approve macFUSE installation and activation on this Mac followed by a
fresh Liquid configure/build. The existing AKS image builder would need an explicit change
to its execution environment to qualify. This audit does not establish that installing
macFUSE is the only possible solution. Installation on this Mac alone would still leave the
second compatible machine and cache-transfer arrangement to establish.

After that decision, repeat the prerequisite checks, identify both machines and matching
toolchain inputs, and record the exact upstream revisions and unchanged source state for
both real projects. Run all five demonstrations with at most six build jobs, retain the
commands and full output, enumerate artifacts, show measured cache hits and timings on the
second machine, verify the attestations, and preserve the failing sampled-verification output
from a separately documented non-determinism injection. There is no validated end-to-end
rerun recipe yet; filling in those measurements is the remaining work of #299. An empty-cache
`verify --all` run or a suite that skips FUSE tests cannot replace them.

Native static/shared suites were not built or run after this prerequisite stop. This result
does not exercise the separate FSEvents (#282) or offline CLI (#296) failures. The issue and
epic remain open; no issue comment or epic update was made.

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

The CLI documentation check extracts the backticked command cell from each Markdown table row.
It must not split table rows into CMake lists: CMake policy CMP0007 differs on whether the leading
empty field before a Markdown row's first `|` is retained.


Test discovery (`gtest_discover_tests`) runs each test binary once at build time with a 120 s timeout (the CMake default is 5 s, which a loaded host exceeds and reports as a build failure).
