# Worker fleet tests

`test_scheduler.cpp` pins the private worker protocol's schedule payload and empty idle response,
action-digest deduplication, recorded-size-class placement, and the one-requeue rule after a
worker lease expires. The scheduler and protocol sources are compiled into the test executable so
private symbols remain outside the public ABI in static and shared builds.

`run-docker-proof.sh <build-directory>` builds the scheduler and worker images, starts two
host-network workers, runs the slow output fixture locally and remotely, checks identical output
digests, and then kills the active worker. The surviving worker must finish the single requeue and
the scheduler metrics must report exactly one retry. `client-proof.sh` runs inside the prescribed
privileged `liquid-linux-ci` image and verifies that `why --json` records the remote cost decision.
Health and metrics probes run from the scheduler container, so they exercise the same host network
even when the container runtime does not publish that network into its desktop host.
The proof uses a two-second worker poll, an eight-second lease, and an explicit client deadline so
the setup exercises heartbeats without saturating the one-request gRPC transport. It collects local
timing history before starting the scheduler. Each client action enters the queue before both
workers start; the loss case restarts the fleet, kills the active worker, and leaves the other to
claim the single requeue. The loss case uses a snapshot of the seeded client cache so its changed
input retains the same four-sample placement history without reusing the baseline operation.

`fixture/slow_copy.c` is both the deterministic toolchain input and the long action. It sleeps
before copying its declared input to its declared output, making worker loss observable. The wrap
conversion records its loader and C runtime in the Phase 2 toolchain, so the worker does not use
runtime files from the worker image.
