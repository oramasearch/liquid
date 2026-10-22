# Build verification

Local and continuous-integration build verification for liquid itself.

| File | What it contains |
|---|---|
| [`local.md`](local.md) | Building liquid on a workstation: prerequisites, configure, build, test, run, install, options, and the documentation checks. |
| [`service-images.md`](service-images.md) | Non-root runtime service images, state permissions, loopback control listeners, and image inspection proof. |
| [`socket-fixtures.md`](socket-fixtures.md) | Unix-domain socket byte limits, scoped temporary paths, cleanup, diagnostics, and deep-worktree verification. |
| [`ci.md`](ci.md) | The GitHub Actions platform matrix, sanitizer coverage, toolchains, offline CLI host prerequisites and guide fixtures, the documentation-site job, and the local Linux container proof. |
| [`phase-2-exit-lane-audit.md`](phase-2-exit-lane-audit.md) | Why the Phase 2 exit of issue #299 has not been demonstrated: every candidate machine checked for a FUSE mount on 2026-09-08 and again on 2026-10-12, the commands and their output, and the decision still needed. |
| [`sandboxed-executor.md`](sandboxed-executor.md) | Issue #10's local executor checkpoint: fixed environment, observed-mount manifest, Linux namespace enforcement, platform limits, and the mount-proof dependency on #130. |
| [`self-host-hang.txt`](self-host-hang.txt) | Debugger output captured during the self-host hang investigation, kept as evidence rather than as documentation. |

[Ordinary-repository projection](project-ordinary-repository.md) records issue #286’s before/after
commands, executable guide, result-ID compatibility, and native verification.
