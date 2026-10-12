# Phase 2 exit: the lane audit for issue #299

The Phase 2 exit asks liquid to wrap an unmodified project, reuse its cache on a second machine,
attest the result, and catch injected non-determinism
([#299](https://github.com/oramasearch/liquid/issues/299)). This page records why that
demonstration has not run: **no machine reachable from the development Mac can run a wrapped
build**, because a wrapped build observes its tools through a FUSE mount and none of the
candidates has one. It is a dated record of what was checked, so the next person does not repeat
the search.

The same prerequisite blocks the Phase 4 exit
([#300](https://github.com/oramasearch/liquid/issues/300)), whose incremental build runs through
`wrap`, and the libfuse baseline of the VFS conformance cases
([`../library/vfs.md`](../library/vfs.md#backend-conformance-301)).

## Status on 2026-10-12

Each finding of the original audit was checked again when `v0.1.0` was cut. All of them still
hold.

| Candidate | 2026-09-08 | 2026-10-12 |
|---|---|---|
| The development Mac | macFUSE absent | Still absent: no `/Library/Filesystems/macfuse.fs`, and `pkg-config` finds no `fuse`. The release's test run skipped its 12 FUSE-dependent and peer-dependent tests. |
| Cluster BuildKit worker | `/dev/fuse` absent | Still absent, on the same pod (`buildkit/aks-amd640-b8cc87cfb-rdt7h`, `moby/buildkit:buildx-stable-1-rootless`, not privileged). `fusermount3` is installed, which does not give access to a device that is not there. |
| Namespace `ci` on the cluster | No pods | The namespace does not exist. |
| Native Linux host `192.168.123.175` | SSH timed out | Still unreachable from this network: SSH timed out, and the local cluster's API on the same host answers "host is down". |
| `codex-work/tools/linux-proof.sh` | Absent | Still absent. |
| GitHub Actions | Fails at private-submodule checkout | Still fails there. The repository has no `ORAMA_SUBMODULES_TOKEN` secret, so the checkout falls back to a token that can read only this repository. |

One thing is new since the audit. The cluster BuildKit lane now builds liquid and runs its tests
on Linux in a container build (liquid-bridge's `liquid-dev` skill). It has no `/dev/fuse` either,
so it does not run the wrapped-build cases, and the whole suite does not pass there yet
([#348](https://github.com/oramasearch/liquid/issues/348),
[#356](https://github.com/oramasearch/liquid/issues/356)).

**The decision the audit asked for is still open**: a Linux machine with a usable `/dev/fuse`,
or macFUSE on the development Mac, and a second compatible machine for the cache-transfer
clause.

## The audit of 2026-09-08

The text below is the audit as written on that date, apart from the headings.

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

## Does macOS have a path without macFUSE?

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

## Off-host Linux checks

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

## Missing evidence and the decision needed

The audit stops here under issue #299's work order. The five demonstrations remain unrun:

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
epic remain open. The finding was summarised in a comment on
[#299](https://github.com/oramasearch/liquid/issues/299) the same day.
