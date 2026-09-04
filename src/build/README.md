# Build implementation

`executor.cpp` owns action validation, sandbox launch, observed-read capture, output collection, and
signed result creation. `cache.cpp` owns candidate-key derivation, current-tree manifest validation,
signature and referenced-object checks, the sorted disk action index, and its store-backed recovery
root. Divergence and the signed verification reference are durable index fields, and lookup never
serves a divergent row. `materialise.cpp` reconstructs toolchain and replay trees from store
records. `verify.cpp` owns sampling, fresh-sandbox replay, digest comparison, attestations, alerts,
and background scheduling.

`wrap.cpp` snapshots an existing working tree, owns its persistent project cache and toolchain pin,
validates saved actions, launches one writable observed mount, derives child actions from process
reads and writes, publishes the driver's outer action, and materialises hits back into the checkout.
Action boundaries are output-producing leaf processes from the discovered toolchain, not executable
names. Completed output snapshots make each recorded action independently replayable.

The private `repository_access.hpp` adapter is the only route from these components to repository
storage, signing, cache and verification configuration, leases, counters, and critical alerts.
Public build values and operations stay
in `include/liquid/build.hpp`.
