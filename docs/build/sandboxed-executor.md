# Sandboxed executor

## Status

Issue #10 supplies the first executable Phase 2 action path. It persists canonical `action`,
`sandbox`, and `result` records; materialises a content-addressed toolchain; runs actions from the
fixed virtual `/liquid` layout; records input reads from the VFS request stream; stores declared
outputs, stdout, stderr, and the read manifest; and returns the decoded result through
`liquid::build`.

Linux launches through unprivileged user, mount, pid, and network namespaces. The process group
is killed on timeout. The namespace contains only the mounted input tree, the materialised
toolchain, private home and temporary directories, declared output bind mounts, and `/proc`; a
read such as `/etc/passwd` is therefore denied by the namespace. The VFS observation callback
receives the FUSE request pid and operation, so the executor does not use process tracing or a
library interposer to construct the manifest.

macOS builds the `sandbox-exec` profile, including the deprecated tool's read and network-denial
rules, but cannot execute an action until macFUSE provides a fixed `/liquid` mount location. The
profile remains present so macFUSE integration has one platform enforcement definition. Windows
returns `errc::unsupported` with the Phase 6 Cloud Files and job-object explanation.

## Verification

The privileged Ubuntu 24.04 FUSE container runs the observed-mount executor tests as real FUSE
requests. They cover an input copied to a declared output, an undeclared input appearing in the
manifest, a denied read outside the namespace, a denied write outside declared outputs, the fixed
environment including `SOURCE_DATE_EPOCH`, repeatable pure-action fields, and timeout process-group
termination. The same run exercises `VfsMount.hydrates_captures_and_reuses_cache`.

On a host without macFUSE the static and shared suites build and pass, while mount-backed action
tests are unavailable because the optional macFUSE target is not configured. This is a platform
coverage limitation, not an observation fallback: the Linux container is the required proof for
the mount request path.
