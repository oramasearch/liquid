# Build implementation

`executor.cpp` owns action validation, sandbox launch, observed-read capture, output collection, and
signed result creation. `cache.cpp` owns candidate-key derivation, current-tree manifest validation,
signature and referenced-object checks, the sorted disk action index, and its store-backed recovery
root. Divergence and the signed verification reference are durable index fields, and lookup never
serves a divergent row. `materialise.cpp` reconstructs toolchain and replay trees from store
records. `verify.cpp` owns sampling, fresh-sandbox replay, digest comparison, attestations, alerts,
and background scheduling.
signed result creation. `cache.cpp` owns candidate-key derivation, current-tree manifest validation,
signature and referenced-object checks, the sorted disk action index, and its store-backed recovery
root. Divergence and the signed verification reference are durable index fields, and lookup never
serves a divergent row. `materialise.cpp` reconstructs toolchain and replay trees from store
records. `verify.cpp` owns sampling, fresh-sandbox replay, digest comparison, attestations, alerts,
and background scheduling. `provenance.cpp` derives the SLSA predicate from each action/result pair,
signs and stores its TAAP attestation, binds it to the signed result, verifies that graph, and writes
native or DSSE/in-toto JSON. `cache.cpp` also verifies result provenance and owns output-path lookup.

`wrap.cpp` snapshots an existing working tree, uses the persistent project cache and toolchain pin,
validates saved actions, launches one writable observed mount, derives child actions from process
reads and writes, publishes the driver's outer action, and materialises hits back into the checkout.
Action boundaries are output-producing leaf processes from the discovered toolchain, not executable
names. Completed output snapshots make each recorded action independently replayable.
names. Completed output snapshots make each recorded action independently replayable.

The private `repository_access.hpp` adapter is the only route from these components to repository
storage, signing, cache and verification configuration, leases, counters, and critical alerts. Public
build values and operations stay
in `include/liquid/build.hpp`.

`why.cpp` resolves output paths, action digests, and result digests through the persistent lookup
journal and action index. It classifies differences strictly from action/result records, manifests,
stored trees and blobs, and repository history. It also owns the completed-wrap statistics
projection. `cache.cpp` records lookup result relationships but does not store a guessed cause.
