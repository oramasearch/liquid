# Worker fleet containers

The two Dockerfiles package the Linux `liquid` executable with distinct scheduler and worker
entry points. The default build context expects `build-linux/liquid`; override
`LIQUID_BINARY` when the executable is elsewhere, `LIQUID_READ_GUARD` for its matching static read
guard, and `LIQUID_BASE` when the prescribed `liquid-linux-ci` image has another local tag.
The root `.dockerignore` limits this image context to the default `build-linux` artifacts and the
acceptance proof's `build/linux` artifacts.

```sh
docker compose -f deploy/workers/compose.yaml build
docker compose -f deploy/workers/compose.yaml up
```

The Compose example uses host networking because that is the acceptance environment. It mounts a
separate liquid store for each process and gives workers the namespace and FUSE privileges needed
by the Phase 2 executor. Scheduler control is on port 8080 and the workers use 8081 and 8082.

Each worker signs its results with its own TAAP identity, and the scheduler accepts a result only
from a worker whose identity chain it trusts (liquid#383). After the workers have started once,
export each worker's chain with `liquid identity show --chain` inside its container and run
`liquid identity trust <chain>` inside the scheduler's container (and on every client that imports
results); until then completions are refused as an unknown signer.

Production credentials, certificates, identity volumes, scheduling policy, and rollout belong to
the Phase 6 operator. Configuration remains flags or the environment variables documented in
[`../../docs/library/workers.md`](../../docs/library/workers.md).
