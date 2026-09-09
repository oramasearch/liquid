# Build verification

Local and continuous-integration build verification for liquid itself.

| File | What it contains |
|---|---|
| [`local.md`](local.md) | Building liquid on a workstation: prerequisites, configure, build, test, run, install, options, and the documentation checks. |
| [`socket-fixtures.md`](socket-fixtures.md) | Unix-domain socket byte limits, scoped temporary paths, cleanup, diagnostics, and deep-worktree verification. |
| [`ci.md`](ci.md) | The GitHub Actions platform matrix, sanitizer coverage, toolchains, the documentation-site job, and the local Linux container proof. |
| [`sandboxed-executor.md`](sandboxed-executor.md) | Issue #10's local executor checkpoint: fixed environment, observed-mount manifest, Linux namespace enforcement, platform limits, and the mount-proof dependency on #130. |
| [`self-host-hang.txt`](self-host-hang.txt) | Debugger output captured during the self-host hang investigation, kept as evidence rather than as documentation. |
