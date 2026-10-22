# Worker fleet tests

The Docker proof prepares its bind-mounted state for UID/GID 65532 before starting services.
HTTP probes run in a separate CI image, so the scheduler and worker runtime images need no
download client or build toolchain. The services retain their non-root image user.

`test_scheduler.cpp` pins the private worker protocol's schedule payload and empty idle response,
action-digest deduplication, recorded-size-class placement, and the one-requeue rule after a
worker lease expires. `WorkerAuthentication.*` (liquid #416) creates separate TAAP identities in
their own `LIQUID_HOME` fixtures and checks that an untrusted worker, a trusted identifier signed by
another key, an unsigned request, a replayed or stale request, a guessed token, and a result signed
by another key are refused, and that the worker's lease check refuses an unsigned, foreign,
tampered, or replayed lease. The scheduler and protocol sources are compiled into the test executable so
private symbols remain outside the public ABI in static and shared builds.

`run-docker-proof.sh <build-directory>` builds the scheduler and worker images, creates an identity
for the scheduler and each worker in its own volume, pins each side's document on the other, starts
two host-network workers, runs the slow output fixture locally and remotely, checks identical output
digests, and then kills the active worker. The surviving worker must finish the single requeue and
the scheduler metrics must report exactly one retry. `client-proof.sh` runs inside the prescribed
privileged `liquid-linux-ci` image and verifies that `why --json` records the remote cost decision.
Health and metrics probes run from the scheduler container, so they exercise the same host network
even when the container runtime does not publish that network into its desktop host.
The proof uses a two-second worker poll, an eight-second lease, and an explicit client deadline so
the setup exercises heartbeats without saturating the one-request gRPC transport. It collects local
timing history before starting the scheduler. Both workers are ready before each client submits;
the loss case restarts the fleet, kills the active worker, and leaves the other to claim the single
requeue. The loss case uses a snapshot of the seeded client cache so its changed input retains the
same four-sample placement history without reusing the baseline operation.

`fixture/slow_copy.c` is both the deterministic toolchain input and the long action. It sleeps
before copying its declared input to its declared output, making worker loss observable. The wrap
conversion records its loader and C runtime in the Phase 2 toolchain, so the worker does not use
runtime files from the worker image.
