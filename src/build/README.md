# Build implementation

`executor.cpp` owns action validation, sandbox launch, observed-read capture, output collection, and
signed result creation. `cache.cpp` owns candidate-key derivation, current-tree manifest validation,
signature and referenced-object checks, the sorted disk action index, and its store-backed recovery
root. `materialise.cpp` reconstructs toolchain trees from store records.

The private `repository_access.hpp` adapter is the only route from these components to repository
storage, signing, cache configuration, leases, and counters. Public build values and operations stay
in `include/liquid/build.hpp`.
