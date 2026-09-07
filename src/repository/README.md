# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
or caller-owned shared store. A shared-store path argument is hashed into a repository-specific
state directory; the handle borrows the store, and repository, directory, ref, and result lease
owners carry the repository identity. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.
Repository retention lease restoration uses the caller-owned store's selected persistence backend,
so shared-store lease generations follow the same durability contract as pack and index metadata.

Issue #86 validates caller-stated write bases before blob registration, content storage, or CRDT
mutation. Persisted `config.agents` selects the TAAP actors that must supply a base. Version 2
signed drip bodies carry the liquid-selected base, touched paths, optional `reasoned_from`, and an
optional disjoint or forced result; the reader retains version 1 support. Stale explanation uses
the shared text-hunk implementation against historical and current blob records and appends no
drip.

The repository document persists `config.flow`. Process-shared controllers enforce actor,
containing-directory, and repository buckets immediately before a CRDT transaction opens;
oversized updates and exhausted buckets return without an operation or drip. Live balances and
rejection counters are not persisted.

Public tree comparison flattens immutable tree records into repository-relative paths, matches
entries by `node_id`, and reports moves independently from mode and content changes. Change-level
comparison also attaches the conflict values stored on each change. Text hunk generation reads
blob contents only when the caller opts in.
Structural source comparison asks the supplied frontend graph for one canonical parse per blob,
matches syntax nodes through unit anchors, subtree hashes, containers, and order, and publishes
explicit structural, text, or binary tiers. Whitespace-only changes remain visible as
`formatting_only`, while comments have their own edit kind.

Unresolved text, composition, drive, and CRDT outcomes use one canonical conflict record. A cut
places its digest in the tree and version-3 change while the repository checkpoint retains the
selected working entry for reads and reopen. Resolution cuts a change that binds the conflict
digest to the selected value. Sync imports rebuild this index from the accepted `main` change.

Text-anchor mapping reuses one change-level text diff, indexes anchors by their source paths, and
walks each matched path's line hunks in order. It follows renames through `node_id`, shifts lines
for earlier count changes, marks replacements approximate, and marks removed regions deleted.

Per-line attribution also follows the file's stable `node_id`. It walks first-parent changes in
oldest-first order, applies text hunks to carry unchanged ownership forward, and binds changed
lines to the matching operation group and session. Stored operation records carry the delegated
agent marker; older documents decode an absent marker as a human operation.

At every cut and configured drip interval, cached directory documents are compact-saved and
immediately load-checked. A state that does not round-trip in compact form falls back to complete
upstream history bytes. Stored documents reopen sync-capable. The repository marker is published
before the covered journal state is retired; open restores later journaled working-tree updates.
Checkpoints with pending drips reconcile reachable parent documents; a no-op open checkpoint keeps
untouched directory documents lazy.
Each checkout path contributes the device component of its automerge actor ID, preventing cloned
repositories from authoring contradictory concurrent sequences under one actor.
Sync import reconstructs a full-history candidate before receiving peer messages, so retries remain
valid across either checkpoint encoding. A receiver that becomes an active directory document uses
the destination repository's actor rather than the transport's shared receiver identity, including
when an empty repository adopts a remote identity.

The build adapter uses the repository's device signer for result records and reproducibility
attestations. It reads persisted cache, verification, and retention policy, synchronizes named ref
and result leases across reopen, retains the action-index root, and owns per-handle cache counters
and critical alerts exposed by `repository::cache_stats`.
`repository::build` starts the same event-producing action path exposed in `liquid/build.hpp`; the
repository must outlive its build handle because execution continues against its store and signer.
The `build_target` overload plans the recorded graph and starts every selected action, while
`root_digest` returns the current stored tree record for public action construction.
`register_test`, `tests`, and `run_tests` delegate native target storage, sorted discovery, and
cached execution to `src/build/tests.cpp`. `open_build` resolves the persistent repository owned by
an observed working-tree build so CLI test runs use the same target and action state as `wrap`.

For build diagnostics, the same adapter resolves a path's current content to the newest matching
write operation and signed drip. Content, node, actor, and timestamp must all agree; otherwise the
caller receives no attribution.

Read-only build replay also asks the private VFS adapter to traverse a recorded tree digest
directly. Entry and child lookup decode that immutable tree graph without changing or consulting
the repository's current working tree, including each remote blob's content identity and location.

The repository document also persists typed `config.policy` rules. `repository::set_policy`
validates the complete replacement before checkpointing it, while the build adapter exposes the
current policy and the store's subject-attestation index to the policy evaluator.
`repository::store_attestation` imports canonical signed evidence without granting it trust;
evaluation verifies the stored record independently.

Retention rules are canonical repository-document configuration. Repository collection checkpoints
first, applies ref and result policy, releases results selected by the physical size cap, and only
then delegates byte reclamation to the store collector. `repository::retain_explain` uses a dry
collector pass so the CLI does not inspect repository internals.

The repository document persists `config.remote`: endpoint, instance, stock REAPI or liquid
scheduler backend, TLS trust and client-key paths, request headers, deadline, batch limit, and
read-only-by-default publication. The public
accessor lets later wrap runs reuse the configuration without exposing document internals.

The document also persists `config.data.databases` and private live branch state. Public status
projects only branch IDs, sessions, declared names, branch URLs, mechanisms, and creation times;
the configured base URL and branch-point fingerprint remain private. The data adapter checkpoints
record addition or removal only after the corresponding PostgreSQL lifecycle step succeeds.
The repository document also persists `config.hot_update`: the sandboxed restart-hook argument
vector and the build-target/local-endpoint pairs consumed by the daemon. Creation and open reject
empty hook arguments, duplicate target names, empty endpoints, or configured targets without a
restart hook. The public accessor returns the effective stored configuration.

The repository document persists `config.deploy`: one operator control-plane URL, named
environment domain, variable, and opaque sealed-secret-reference settings, plus the optional
preview-on-push policy. The public accessor returns this configuration without interpreting or
resolving secret references.

The private search adapter exposes only the repository checkout's `.liquid/index/` path to the
search implementation. Public queries remain on `repository::search`; no index representation or
oramacore type crosses the public boundary.

Remote file entries use the same working tree and canonical blob reference as local files.
`store_remote_blob` registers metadata without reading content, `write_remote` records the entry,
and repository status recursively sums chunk bytes that are not yet local.
The private VFS adapter can register a runtime remote source on the repository's store, allowing a
drive mount to resolve metadata-only sync records without exposing store internals publicly.

Sync export pages signed drips, changes, per-directory upstream messages, refs, and content roots.
Import works on repository and directory candidates, verifies TAAP signatures and checked CRDT
receives, and publishes one checkpoint only after the complete batch passes. An empty repository
may adopt the incoming repository identity; a non-empty unrelated repository is rejected.

Exclusive checkout persists named path-pattern rules and holder, time, reason, and authority
records. Mutations allow the holder and reject a different actor with the complete lock details.
Acquire, release, and rule replacement use signed version-4 drips. Import verifies each lock holder
against the signing actor and turns concurrent different-holder records into issue #40 conflict
values while retaining the local claim. An injected authorizer runs before connected acquire or
release changes local state.

Every document mutation records its operation, session, and before/after effects. Operation undo
and filtered session undo derive inverses from those effects, skip state changed by newer intent,
and append their results without rewriting source or imported records. Each touched directory uses
one candidate transaction, and a failed plan, transaction, encoding, or journal publication
restores all active state. Change version 2 also records document state and directory heads;
historical document reads validate those heads against full archives without consulting active
shallow documents.

`repository::store_symbols` stores one canonical symbol record and its source-blob association.
`repository::symbols` resolves and decodes that content-addressed value.

Session forks retain private directory documents and journal state on the caller-owned store until
publish coalesces their net directory effects into base-checked shared drips. Repository metadata
also records semantic leases, expiring presence, and acknowledged checkpoint epochs.

Structural-edit preview reads those working-tree blobs and symbol records, producing exact
range-local replacements and refusal records. Apply validates every before digest before the
first write, replays only the previewed replacements through `repository::write`, checks after
digests, and groups all per-file effects into one attributed `structural_edit` operation for undo.
The private drive adapter stages filesystem files through bounded store streaming, exposes tree
snapshots and content identities to the three-tree reconciler, records selected existing blob
references without copying their bytes, merges pulled directory documents before reconciliation,
records local and remote parents when they differ, and publishes files atomically. Drive state
itself remains repository metadata, and its remote shadow repository borrows the same object store.
