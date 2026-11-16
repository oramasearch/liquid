# Worker fleet containers

The two Dockerfiles package the Linux `liquid` executable with distinct scheduler and worker
entry points. The default build context expects `build-linux/liquid`; override
`LIQUID_BINARY` when the executable is elsewhere, `LIQUID_READ_GUARD` for its matching static read
guard, and `LIQUID_BASE` when the prescribed `liquid-linux-ci` image has another local tag.
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
The scheduler's REAPI listener binds `127.0.0.1:50051`, which the host-network workers reach. A
scheduler on any other address is refused unless it is given `--tls-cert`, `--tls-key`, and
`--token-file` or `--client-ca`, and its workers then need `--scheduler-ca` (or
`--scheduler-tls`) and `--token-file` or a client certificate (liquid #520).

Each worker signs its results with its own TAAP identity, and the scheduler accepts a result only
from a worker whose identity chain it trusts (liquid#383). After the workers have started once,
export each worker's chain with `liquid identity show --chain` inside its container and run
`liquid identity trust <chain>` inside the scheduler's container (and on every client that imports
results); until then completions are refused as an unknown signer.

Production credentials, certificates, identity volumes, scheduling policy, and rollout belong to
the Phase 6 operator. Configuration remains flags or the environment variables documented in
Production identity provisioning (creating each identity, distributing the documents, rotating
them), scheduling policy, and rollout belong to the Phase 6 operator. Configuration remains flags or the environment variables documented in
[`../../docs/library/workers.md`](../../docs/library/workers.md).
