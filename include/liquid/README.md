# Public headers

Installed C++ headers in this directory define libliquid's supported API. `liquid.hpp` is the
umbrella include; individual headers are independently compilable. The detailed status and API
inventory is maintained in `docs/library/api-map.md`.

`object.hpp` defines the canonical conflict record, byte and node regions, conflict references,
and change-resolution references. `repository.hpp` exposes unresolved inspection, producer
recording, and resolution operations; all waiting operations remain coroutine-native.

`frontend_abi.h` defines both language frontend modules and the versioned native liquify plugin
descriptor. Editor plugins reuse the frontend boundary's canonical-CBOR bytes, owned results,
errors, progress, and cancellation types while registering queries, commands, and panels.
`tiers.hpp` defines the pure support matrix projected from the process-global frontend registry,
the built-in format registry, and the declared platform backends.
errors, progress, and cancellation types while registering queries, commands, panels, and a
versioned presence, lease, and attribution event table.

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
claims, transient presence, epoch compaction reports, and per-line actor/session/change
attribution records used by the repository and daemon APIs.
`tuf.hpp` defines the repository-backed trusted-root bootstrap, ordered metadata update verifier,
and complete target-byte verification gate used by registry and drive clients.

`abi.hpp` defines synchronous linked-binary extraction and diffing values. Repository-backed ABI
lookup remains asynchronous in `repository.hpp`, and wrapper compatibility events are part of
`build.hpp`. Private ELF, Mach-O, and PE reader details do not enter the installed headers.
`build.hpp` also exposes relocation findings, reports, the asynchronous output-tree check, and
install-prefix wrapper options. Binary-reader and repository-scanning details remain private.
`projection.hpp` exposes deterministic package, OCI, Helm, OSTree, and bootc targets. OSTree and
bootc sources carry optional relative mount paths, options select the ref and repository mode, and
the report returns the OSTree commit independently of an OCI image digest.

`admission.hpp` defines the versioned image-admission policy and CBOR/JSON decision record,
rule-attributed review results, per-policy log mode and trust anchors, detailed transparency
outcomes, injected attestation-source and transparency-log interfaces, the one-image review
operation, named policy-directory options, bounded replay, and the long-running admission server.
`identity.hpp` exposes algorithm- and custody-tagged actors, software and hardware key rotation,
and TPM-capable key custody without exposing transport types. `release.hpp` exposes signed release
policy and approval values plus distinct-identity threshold verification.
`resolve.hpp` defines the package and concretised-spec records, pure spec parser and canonical
codecs, coroutine-native universe loading and solving, and stable explanation values. The CDCL
engine and its clause encoding stay private.
`configuration.hpp` defines the `.liquid` AST, exact diagnostics, synchronous check/lower/format
operations, canonical target/source codecs, asynchronous source fetch, action-backed generators,
configured target execution, and lowered install intents. `install.hpp` defines the closed
`install_intent/1` values and codecs plus atomic generation install, list, and rollback tasks.
`projection.hpp` accepts those intents, exposes fixed lifecycle-script generation, checks package
ownership in batches or against installed inventories, and reports derived replacements.
and configured target execution.
`import.hpp` defines Spack import reports and loss categories, stable text and JSON report codecs,
offline patch-byte injection, optional target toolchain binding, and the coroutine import task.
`deploy.hpp` defines coroutine-native deployment creation, listing, rollback, and environment
status calls for an operator-owned control plane. Its interim image input is an OCI reference
pinned to a SHA-256 digest; the private HTTP client and control-plane JSON stay outside the API.
