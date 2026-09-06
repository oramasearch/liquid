# Public headers

Installed C++ headers in this directory define libliquid's supported API. `liquid.hpp` is the
umbrella include; individual headers are independently compilable. The detailed status and API
inventory is maintained in `docs/library/api-map.md`.

`frontend_abi.h` defines both language frontend modules and the versioned native liquify plugin
descriptor. Editor plugins reuse the frontend boundary's canonical-CBOR bytes, owned results,
errors, progress, and cancellation types while registering queries, commands, and panels.

`sync.hpp` includes metadata-only tree receipt for node-store and other lazy-hydration clients.
It records remote blob locations without fetching content chunks.
`store.hpp` exposes runtime remote-source registration, hydration and remote-access observation,
and live local-byte accounting for long-running node stores.
`node_store.hpp` defines the embedded node chunk-store service, its mount requests, and its status
values.

`abi.hpp` defines synchronous linked-binary extraction and diffing values. Repository-backed ABI
lookup remains asynchronous in `repository.hpp`, and wrapper compatibility events are part of
`build.hpp`. Private ELF, Mach-O, and PE reader details do not enter the installed headers.
`build.hpp` also exposes relocation findings, reports, the asynchronous output-tree check, and
install-prefix wrapper options. Binary-reader and repository-scanning details remain private.

`admission.hpp` defines the versioned image-admission policy, injected attestation-source and
transparency-log interfaces, the one-image review operation, and the long-running admission server.
`resolve.hpp` defines the package and concretised-spec records, pure spec parser and canonical
codecs, coroutine-native universe loading and solving, and stable explanation values. The CDCL
engine and its clause encoding stay private.
