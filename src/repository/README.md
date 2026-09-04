# Repository implementation

Repository mutations update the working tree, directory documents, signed drip bodies, and local
or caller-owned shared store. A shared-store path argument is hashed into a repository-specific
state directory; the handle borrows the store, and repository, directory, ref, and result lease
owners carry the repository identity. Redaction persists the caller's replacement and a signed marker before targeted store
tombstoning, pack compaction, and the required `not_found` verification.

At every cut and configured drip interval, cached directory documents are compact-saved, stored,
and immediately reopened shallowly. The repository marker is published before the covered journal
state is retired; open restores later journaled working-tree updates on the shallow documents.
Each checkout path contributes the device component of its automerge actor ID, preventing cloned
repositories from authoring contradictory concurrent sequences under one actor.
Sync import reconstructs a full-history candidate from each compact checkpoint before receiving
peer messages, so retries remain valid even though active directory documents are shallow.

The build adapter uses the repository's device signer for result records and reproducibility
attestations. It reads persisted cache, verification, and retention policy, synchronizes named ref
and result leases across reopen, retains the action-index root, and owns per-handle cache counters
and critical alerts exposed by `repository::cache_stats`.

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

The repository document persists `config.remote`: endpoint, instance, TLS trust and client-key
paths, request headers, deadline, batch limit, and read-only-by-default publication. The public
accessor lets later wrap runs reuse the configuration without exposing document internals.

Remote file entries use the same working tree and canonical blob reference as local files.
`store_remote_blob` registers metadata without reading content, `write_remote` records the entry,
and repository status recursively sums chunk bytes that are not yet local.

Sync export pages signed drips, changes, per-directory upstream messages, refs, and content roots.
Import works on repository and directory candidates, verifies TAAP signatures and checked CRDT
receives, and publishes one checkpoint only after the complete batch passes. An empty repository
may adopt the incoming repository identity; a non-empty unrelated repository is rejected.
