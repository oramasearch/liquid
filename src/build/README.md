# Build implementation

`executor.cpp` owns action validation, sandbox launch, observed-read capture, output collection, and
signed result creation. It exposes one preparation and one publication path to
`distribution.cpp`, which walks source and toolchain object graphs, measures the remote's missing
objects, selects local, remote, or race from recorded cost inputs, groups short actions into a
supported batch, hydrates remote outputs, and cancels race losers before the winner reaches the
single publication path. `cache.cpp` owns candidate-key derivation, current-tree manifest validation,
signature and referenced-object checks, the sorted disk action index, and its store-backed recovery
root. It also computes the median execution cost, synchronizes cost-weighted result leases, and
ranks size-cap eviction by execution cost per result-graph byte. Divergence and the signed
verification reference are durable index fields, and lookup never serves a divergent row.
`materialise.cpp` reconstructs toolchain and replay trees from store
records. `verify.cpp` owns sampling, fresh-sandbox replay, digest comparison, attestations, alerts,
background scheduling, and the explicit replay API. Verification and replay share reconstruction,
sandbox execution, and byte comparison. Explicit replay uses the repository's registered
`remote_blob_source` to hydrate recorded remote inputs and expected outputs; it records
attestations, alerts, and quarantine only when requested.
`provenance.cpp` derives the SLSA predicate from each action/result pair,
signs and stores its TAAP attestation, binds it to the signed result, verifies that graph, and writes
native or DSSE/in-toto JSON.
`policy.cpp` validates and digests repository policy, walks source and toolchain input trees, verifies
subject-indexed attestations, counts distinct approved issuers, and produces refusal or warning
reports before cache lookup and execution.

`why.cpp` re-evaluates that policy for a recorded action before classifying a hit or ordinary miss,
and reports the first unsatisfied refuse rule as a typed policy cause.

`facts.cpp` owns canonical configure-probe forms, exact toolchain/sysroot/target keys,
content-addressed fact records, index rebuild, Autotools and CMake cache-variable discovery, and
`fact-bundle/1` export. It has no remote-service dependency. `cache.cpp` also verifies result
provenance and SBOMs and owns output-path lookup.
`sbom.cpp` walks producing results to manifest leaves, adds toolchain and nested-repository
components, writes SPDX 2.3 and CycloneDX 1.5 from one model, stores signed `sbom/1` claims, and
performs explicitly marked ELF/Mach-O/PE inference for foreign artifacts.

`wrap.cpp` snapshots an existing working tree, uses the persistent project cache and toolchain pin,
validates saved actions, launches one writable observed mount, derives child actions from process
reads and writes, publishes the driver's outer action, and materialises hits back into the checkout.
Action boundaries are output-producing leaf processes from the discovered toolchain, not executable
names. Completed output snapshots make each recorded action independently replayable. Direct
execution and wrapping demand persisted `action_result` graph nodes before the disk cache. The
wrapper primes a node for every discovered tool action; after an upstream miss, the child hook
checks a condition over the producer's output bytes before accepting a downstream hit, giving
unchanged objects early cutoff without hiding a changed object from the linker.
Refreshed saved actions retain their prior dependency order so invalidated producer outputs are
known before any dependent plan is accepted.
`trace.cpp` derives dependency edges when a result manifest reads another action's output, stores a
`build_trace` record for every wrap, computes longest paths, loads the latest retained trace, and
exports Chrome trace-event JSON including each recorded execution decision and model input. The
wrap hook records monotonic process entry and exit values;
`wrap.cpp` combines them with its hash and store measurements. `cache.cpp` retains the ordered trace
digests in the canonical action-index root.

`enumeration.cpp` implements the repository's cursor pages for results, attestations, retained
traces, and result-scoped SBOM reports. Provenance, diagnostics, SBOM graph discovery, and trace
loading consume those public pages so command and library views share filtering and ordering.

The private `repository_access.hpp` adapter is the only route from these components to repository
storage, signing, cache and verification configuration, leases, counters, and critical alerts. Its
export annotation gives static and shared builds the same internal test surface. Public build values
and operations stay in `include/liquid/build.hpp`.

`why.cpp` resolves output paths, action digests, and result digests through the persistent lookup
journal and action index. It classifies differences strictly from action/result records, manifests,
stored trees and blobs, and repository history. It also owns the completed-wrap statistics
projection with separate disk and REAPI remote tiers, and returns the selected result's immutable
schedule plus tier counts for CLI explanation. `cache.cpp`
records lookup result relationships but does not store a guessed cause.

`record_remote_result` stores signed remote action results and their provenance using registered
version 2 blob records, without reading output content. Remote scheduling uses the shared
`remote_blob_source` contract and `remote_executor` from `liquid/remote.hpp`; the executor passes
remote eligibility to the Linux read guard and returns an unauthorized error containing the first
forbidden read path. `wrap.cpp` checks REAPI ActionCache only after disk, imports under the same
candidate, and passes the imported result through the ordinary signed manifest-validating lookup.
