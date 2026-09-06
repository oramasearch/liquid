# Hot-update implementation

`hot_update.cpp` owns canonical build-evidence and patch records, deterministic tier
classification, signed `hot_update/1` patch verification, and reconstruction of a running
artifact's base-plus-patches composition. Missing frontend units and missing ABI descriptors fail
closed to restart outcomes.

`runtime.c` implements the separately versioned C ABI, local endpoint, loadable-object resolution,
acknowledged quiescent tier-1 entry-slot replacement, and load-only tier-2 additions. Restart hooks
execute through the build sandbox and produce a signed outcome record.

The public contract and decision table are documented in
[`docs/library/hot-update.md`](../../docs/library/hot-update.md).
