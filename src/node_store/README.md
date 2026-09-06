# Node store service

`service.cpp` owns the shared node chunk store, image repositories, read-only VFS mounts,
hydration counters, remote-content recency, and byte-budget enforcement. It also registers the
local snapshotter and TCP operator route tables on the shared HTTP listener implementation under
`src/serve/`. Control route registration is grouped by liveness, status, administration, OpenAPI,
and MCP surfaces so each group remains independently reviewable.

Each image path is created before its caller-owned-store repository. The path is the persistent
marker used to reopen that repository after its mount reference count reaches zero or the service
restarts.

The repository is moved into its final mount-table owner before FUSE starts. The backend's borrowed
repository reference therefore remains stable for the full mount lifetime.

The public value contract remains in `include/liquid/node_store.hpp`; CLI transport code remains
under `src/cli/`.
