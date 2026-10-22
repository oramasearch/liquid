# Worker fleet containers

The two Dockerfiles package the Linux `liquid` executable with distinct scheduler and worker
entry points. The default build context expects `build-linux/liquid`; override
`LIQUID_BINARY` when the executable is elsewhere, `LIQUID_READ_GUARD` for its matching static read
guard. Both images run as `65532:65532` on an Ubuntu 24.04 runtime with no CI toolchain;
`LIQUID_BASE` is no longer supported. The worker also carries the FUSE and namespace utilities its
sandbox uses; action toolchains arrive through the scheduler's content-addressed store.
The root `.dockerignore` limits this image context to the default `build-linux` artifacts and the
acceptance proof's `build/linux` artifacts.

```sh
docker compose -f deploy/workers/compose.yaml build
mkdir -p deploy/workers/trust/workers
for service in scheduler worker-one worker-two; do
  docker compose -f deploy/workers/compose.yaml run --rm --no-deps --entrypoint liquid \
    "$service" identity show --document >"deploy/workers/trust/$service"
done
mv deploy/workers/trust/worker-one deploy/workers/trust/worker-two deploy/workers/trust/workers/
docker compose -f deploy/workers/compose.yaml up
```

The scheduler and the workers authenticate each other (liquid #416). Each process keeps its own
TAAP identity in a `/var/lib/liquid-identity` volume (`LIQUID_HOME`), and each side is given the
other's identity document, the output of `liquid identity show --document`: the scheduler reads
every file in `/etc/liquid/workers` (`--trust-worker`), and each worker reads
`/etc/liquid/scheduler` (`--trust-scheduler`). The loop above creates the three identities in
their volumes and writes the documents to `deploy/workers/trust/`, which Compose mounts read-only
at `/etc/liquid`. Neither process starts without its trust configuration. A worker whose document
is not in the scheduler's directory is refused; a lease not signed by the pinned scheduler stops
the worker. Rotating an identity means replacing its document on the other side.

The Compose example uses host networking because that is the acceptance environment. It mounts a
separate liquid store for each process and gives workers the namespace and FUSE privileges needed
by the Phase 2 executor. Scheduler control is on port 8080 and the workers use 8081 and 8082.
All image control listeners default to loopback. Existing state and identity volumes must be owned
by UID/GID 65532; mounted trust documents must be readable by it. Fresh named volumes inherit the
image's ownership. See the [image contract](../../docs/build/service-images.md) for permissions,
network exposure, and the image inspection proof.

Production identity provisioning (creating each identity, distributing the documents, rotating
them), scheduling policy, and rollout belong to the Phase 6 operator. Configuration remains flags or the environment variables documented in
[`../../docs/library/workers.md`](../../docs/library/workers.md).
