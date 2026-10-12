# Build verification

Local and continuous-integration build verification for liquid itself.

| File | What it contains |
|---|---|
| [`local.md`](local.md) | Building liquid on a workstation: prerequisites, configure, build, test, run, install, options, and the documentation checks. |
| [`ci.md`](ci.md) | The GitHub Actions platform matrix, sanitizer coverage, toolchains, documentation-site job, and Linux proof methods; issue #299's Phase 2 lane audit records the macFUSE requirement, unavailable remote prerequisites, and owner decision needed to proceed. |
| [`sandboxed-executor.md`](sandboxed-executor.md) | Issue #10's local executor checkpoint: fixed environment, observed-mount manifest, Linux namespace enforcement, platform limits, and the mount-proof dependency on #130. |
| [`self-host-hang.txt`](self-host-hang.txt) | Debugger output captured during the self-host hang investigation, kept as evidence rather than as documentation. |
