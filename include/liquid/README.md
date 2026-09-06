# Public headers

Installed C++ headers in this directory define libliquid's supported API. `liquid.hpp` is the
umbrella include; individual headers are independently compilable. The detailed status and API
inventory is maintained in `docs/library/api-map.md`.

`frontend_abi.h` defines both language frontend modules and the versioned native liquify plugin
descriptor. Editor plugins reuse the frontend boundary's canonical-CBOR bytes, owned results,
errors, progress, and cancellation types while registering queries, commands, and panels.

`data.hpp` defines persisted PostgreSQL application-database configuration and the coroutine-native
per-session branch lifecycle. Database client and server implementation types remain private.
`sync.hpp` includes metadata-only tree receipt for node-store and other lazy-hydration clients.
It records remote blob locations without fetching content chunks and exposes the endpoint operation
used by drive clients to import metadata before an on-demand read.
`drive.hpp` defines persisted device policy, three per-path availability states, hydration debt,
pending-upload status, and the pin, unpin, configuration, and status operations.
`vfs.hpp` accepts an optional bearer token so a drive mount can hydrate through its persisted
remote endpoint.
`store.hpp` exposes runtime remote-source registration, hydration and remote-access observation,
and live local-byte accounting for long-running node stores.
`node_store.hpp` defines the embedded node chunk-store service, its mount requests, and its status
values.
`session.hpp` defines private speculative repository replicas, scoped delegation, semantic lease
claims, exclusive checkout rules and records, transient presence, and epoch compaction reports.
`repository.hpp` exposes lock acquisition, holder-only release, configuration, enumeration, and
the optional connected authorization callback. `daemon.hpp` exposes lock requests and complete
lock-set delivery to local targets.
claims, transient presence, and epoch compaction reports.
`tuf.hpp` defines the repository-backed trusted-root bootstrap, ordered metadata update verifier,
and complete target-byte verification gate used by registry and drive clients.

`abi.hpp` defines synchronous linked-binary extraction and diffing values. Repository-backed ABI
lookup remains asynchronous in `repository.hpp`, and wrapper compatibility events are part of
`build.hpp`. Private ELF, Mach-O, and PE reader details do not enter the installed headers.
`build.hpp` also exposes relocation findings, reports, the asynchronous output-tree check, and
install-prefix wrapper options. Binary-reader and repository-scanning details remain private.

`admission.hpp` defines the versioned image-admission policy and CBOR/JSON decision record,
rule-attributed review results, injected attestation-source and transparency-log interfaces, the
one-image review operation, named policy-directory options, bounded replay, and the long-running
admission server.
`resolve.hpp` defines the package and concretised-spec records, pure spec parser and canonical
codecs, coroutine-native universe loading and solving, and stable explanation values. The CDCL
engine and its clause encoding stay private.
`configuration.hpp` defines the `.liquid` AST, exact diagnostics, synchronous check/lower/format
operations, canonical target/source codecs, asynchronous source fetch, action-backed generators,
and configured target execution.
`deploy.hpp` defines coroutine-native deployment creation, listing, rollback, and environment
status calls for an operator-owned control plane. Its interim image input is an OCI reference
pinned to a SHA-256 digest; the private HTTP client and control-plane JSON stay outside the API.
