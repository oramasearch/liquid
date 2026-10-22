# Service runtime images

The admission, node-store, scheduler, and worker recipes under `deploy/` run as UID/GID
`65532:65532`. Each starts from Ubuntu 24.04 and installs runtime libraries with
`--no-install-recommends`; none inherits `liquid-linux-ci`. Admission and scheduler add only
the binary, certificates, and its shared-library dependencies to the runtime base. Node-store adds the FUSE helper;
worker adds FUSE, `unshare`, and `mount` for its existing sandbox. Compilers, development headers,
build systems, Git, and PostgreSQL stay in the build/test image. Workers fetch action toolchains
as content-addressed inputs, independently of the service filesystem.

This addresses the default-template hardening gap recorded in
[liquid #442](https://github.com/oramasearch/liquid/issues/442), finding LSR-202. It makes no claim
about images an external deployment builds or the user it selects at runtime. The CI image
remains a root build/test environment. `LIQUID_BASE` was removed so it cannot accidentally restore
CI-image inheritance; `LIQUID_BINARY` and the worker's `LIQUID_READ_GUARD` still select local
build artifacts. Use Linux binaries built against Ubuntu 24.04-compatible shared libraries;
these recipes package the executable, not a shared libliquid build.

## State and permissions

Every image defines a locked `liquid` system account and group at 65532, with a nologin shell and
`/var/lib/liquid-identity` as its home. A named account is required by the
[FUSE helper's username lookup](https://github.com/libfuse/libfuse/blob/fuse-3.14.0/util/fusermount.c#L625)
when a non-root user mounts a filesystem. The process still selects its numeric UID/GID explicitly.

The executable and the worker's static read guard are root-owned with mode `0555`. Each service's
store directory and `/var/lib/liquid-identity` are owned by `65532:65532`, with mode `0700`, before
`VOLUME` is declared. `LIQUID_HOME` points to that identity directory for every service. Node-store
also owns `/run/liquid` with mode `0755` so it can bind its Unix socket. Fresh Docker named volumes
inherit these owners; existing volumes and host bind mounts must be provisioned with the same
ownership before an upgrade. The image does not change mounted-file ownership at startup.

Mounted policy, serving certificate/key, and pinned identity documents must be readable by the
service user. Provision them through deployment configuration; do not make private keys broadly
readable. The worker and node-store still need FUSE access and the namespaces required by their
existing executor/mount contract. The runtime user remains non-root even when the deployment
grants these facilities. Restricted environments fail the operation instead of starting the
service as root.

FUSE normally restricts file access to the mount owner, as described in the
[kernel mount options](https://docs.kernel.org/filesystems/fuse/fuse.html#mount-options).
Under these default rules, node-store mount consumers must use UID 65532 in a compatible user
namespace. Deployments with a root snapshotter must coordinate that UID transition with their
integration before using this image; changing the image user does not broaden FUSE access.

## Network defaults

All four control listeners bind to `127.0.0.1` by default (ports 8080 through 8083). The admission
webhook still listens on `0.0.0.0:8443`; the scheduler's REAPI listener still uses
`0.0.0.0:50051`. They are the service data listeners. Node-store's data endpoint remains
`unix:///run/liquid/node-store.sock`. Flags can explicitly select another control address; a
deployment doing so must restrict access with its network policy. The host-network worker Compose
example already uses loopback control addresses.

## Verification

The portable regression runs in the default CTest suite as `service-image-templates`:

```sh
python3 tests/build/check-service-templates.py .
ctest --test-dir "$BUILD" --timeout 600 -R service-image-templates --output-on-failure
```

It checks all four recipes for the runtime package allowlist, explicit non-root user, writable
state ownership, read-only artifacts, and loopback controls. Running the same test against the
unfixed recipes from PR #466's `8ccf071` produces failures for every image: the CI base, missing
user/owned state, and public control listeners.

On a Linux Docker build host with `timeout`, place `liquid` and its matching static `liquid-read-guard` in
`build-linux/` in this checkout (the context allowlist accepts those two artifacts), then run:

```sh
docker build -t liquid-lsr202-admission:test -f deploy/admission/Dockerfile --build-arg LIQUID_BINARY=build-linux/liquid .
docker build -t liquid-lsr202-node-store:test -f deploy/node-store/Dockerfile --build-arg LIQUID_BINARY=build-linux/liquid .
docker build -t liquid-lsr202-scheduler:test -f deploy/workers/Dockerfile.scheduler --build-arg LIQUID_BINARY=build-linux/liquid .
docker build -t liquid-lsr202-worker:test -f deploy/workers/Dockerfile.worker --build-arg LIQUID_BINARY=build-linux/liquid --build-arg LIQUID_READ_GUARD=build-linux/liquid-read-guard .
bash tests/build/check-service-images.sh liquid-lsr202
```

The image check requires `Config.User=65532:65532` and verifies the process's actual UID/GID and
named account. It
searches the runtime filesystem for compiler/build-tool executables, checks binary and state modes
and ownership, writes to every state directory and the node socket directory, starts `liquid
version` to detect missing libraries, and creates an identity as the service user. Containers run
without networking, extra capabilities, or privilege escalation. No image is pushed. The separate
privileged worker-fleet proof still covers FUSE and action execution; it does not run in an
unprivileged build runner.
