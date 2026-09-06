# Store implementation

Repository configuration encryption uses the private `repository_detail::store_access` adapter to
choose a plaintext metadata put or an explicit trust-domain encrypted put for whole-file payload
chunks; the public store API keeps its existing default-key behavior.

This directory owns FastCDC chunking, append-only packs, the fixed-width index, persisted local
leases, per-kind reference extraction, and epoch collection. `collector.cpp` preserves the
ordering documented in `docs/library/gc.md`: committed tombstone manifest, index manifest, then
whole-pack removal. `compactor.cpp` rewrites selected partially dead packs by copying physical
frames byte-for-byte, with the collection manifest rename as the commit point. The object map also
provides `store::key_version` with the recorded trust-domain data-key version and verifies whether
the attached keyring can currently unwrap it; key-id parsing stays in `src/identity/key_version.hpp`.

Targeted redaction removes selected object-map entries, commits tombstones for their unshared
frames, rebuilds the index, and compacts only affected packs. It is separate from lease-driven
reachability collection so unrelated repository objects remain available.

Build-cache collection roots are canonical `action-index-root` records plus named result leases
under the same store. The reference registry traverses root to verification attestations and traces,
result to action/output/stdout/stderr/provenance/SBOM attestations, ABI descriptors, and ABI
compatibility attestations, each `sbom/1` attestation to its canonical SBOM record, and action to
input/toolchain/sandbox. The index root retains verification
attestations and trace digests but does not pin results, so result expiry and cost-aware eviction can
release one usable graph without weakening the collector's manifest/index/pack-removal ordering.
Each trace retains its named results, including results from wraps that did not publish cache entries.
Collector explanations carry object-level reasons and count live policy roots by rule name.
`subject_index.cpp` maps an attestation's signed subject digest to its canonical attestation record
digest. The map updates when records are stored and rebuilds from attestation objects when the store
opens; build-input policy still loads and verifies every indexed record before counting it. The
private object-kind listing used by repository record pages returns canonical object IDs without
exposing store internals in the public API.

Repository internals can query the opened store's root and configuration without exposing either
through the public store API. This places per-repository checkpoint and journal entry points under
`repositories/` and creates repository-identified lease owners while all immutable objects remain
in the common packs and index.

Remote blob registrations are persisted separately in `packs/remotes.meta`. The store reports
whole-content availability as absent, remote, or local and fetches only missing logical chunks
through the configured source before verifying and writing them through the ordinary pack path.
Once a chunk is local, reads load that verified chunk directly; this also handles one-chunk blobs
whose content and chunk digests are identical without resolving the remote registration again.
Verified sync insertion selects the transmitted digest algorithm after checking the bytes, while
ordinary local insertion continues to use the store configuration.

The optional `blob_write_through` receives the complete content digest, byte span, and ordered
chunk table once before any pack mutation. Memory puts retain their input across the await. File
puts first scan in bounded windows, verify a read-only file mapping against that table, and retain
the mapping across the await before copying one chunk at a time into packs.

`mutation_mutex.hpp` owns the store's mutation lock: an asynchronous mutex that is safe from any
thread, following Capy's IoAwaitable protocol so call sites read as with `async_mutex::scoped_lock`.
Capy's `async_mutex` is single-executor, liquid runs every `io_context` on several threads, and
Corosio resumes a coroutine after file I/O on the completing runner, so the lock protects its own
state. Collection releases the mutation lock while marking and yields between packs with
`runtime::yield` rather than a zero delay (liquid #208); its sweep reloads the committed manifest
under the lock and keeps tombstones added by an intervening eviction or redaction.

Remote-registered eviction removes the local object entry, adds tombstones for unshared cached
chunks and its descriptor, and rebuilds the index while preserving `remotes.meta`. It does not
rewrite packs; ordinary collection and compaction reclaim their dead frames. Registrations already
remote-only make eviction a successful no-op, while unregistered content returns `not_remote`.

REAPI negotiation may require SHA-256 objects in a repository whose ordinary identity is BLAKE3.
The repository and REAPI adapters can insert verified raw objects under an explicit supported
algorithm; the object map and pack path retain both identities without changing the store default.

Delta records are detected as their own kind and retain source, target, and patch-blob digests.
`delta/1` attestations retain their delta record, and the subject index provides the signed target
catalog. Delta fetch uses ordinary registered remote blobs for a missing patch or target.
ABI descriptors are detected as their own canonical kind and retain their subject library blobs.
Compatibility attestations retain their preceding library blobs; both record kinds remain reachable
through result references and the subject index.
