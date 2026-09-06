# Node store service

`service.cpp` owns the shared node chunk store, image repositories, read-only VFS mounts,
hydration counters, remote-content recency, and byte-budget enforcement. It also registers the
local snapshotter and TCP operator route tables on the shared HTTP listener implementation under
`src/serve/`.

The public value contract remains in `include/liquid/node_store.hpp`; CLI transport code remains
under `src/cli/`.
