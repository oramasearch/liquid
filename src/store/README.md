# Store implementation

Tree reachability includes each nested repository's pinned change as well as its tree, preserving
nested history during composition transfers and collection.

Repository configuration encryption uses the private `repository_detail::store_access` adapter to
choose a plaintext metadata put or an explicit trust-domain encrypted put for whole-file payload
chunks; the public store API keeps its existing default-key behavior.

`index.cpp` builds each run's 256 KiB fanout table in a heap vector, never as a coroutine local:
a Debug coroutine ramp reserves stack for its locals (liquid #323).

This directory owns FastCDC chunking, append-only packs, the fixed-width index, persisted backend
leases, per-kind reference extraction, and epoch collection. `collector.cpp` preserves the
ordering documented in `docs/library/gc.md`: published object map, committed tombstone manifest,
index manifest, then whole-pack removal. The map precedes the tombstones so no published object
ever names a tombstoned chunk (liquid #337). `compactor.cpp` rewrites selected partially dead packs by copying physical
frames byte-for-byte, with the atomic collection-manifest put and durability barrier as the commit
point. The object map also
provides `store::key_version` with the recorded trust-domain data-key version and verifies whether
the attached keyring can currently unwrap it; key-id parsing stays in `src/identity/key_version.hpp`.

`chunker.cpp` computes a rolling-shingle resemblance hash alongside FastCDC. `store_impl.cpp`
persists those hashes in `LQOBJ5`, keeps a path-scoped 256-entry/16-MiB candidate cache, and caps
delta chains at eight. Candidate selection uses retained raw bytes and never performs a base read
or hash on the write path. `pack.cpp` stores dictionary-compressed targets with a digest-addressed
base while retaining the fixed pack table. The collector marks transitive bases; repack mode
rewrites old deltas against their oldest useful base through the ordinary compaction transaction.
`backend.cpp` is the disk reference implementation of the public persistence contract. Store
metadata, pack appends and sealing, mapped index runs, leases, collection, and compaction use
logical backend keys. Disk runs retain the mapped-file fast path; non-disk runs are held in owned
memory after a backend range read. Every blocking filesystem metadata operation uses
`runtime::offload`.

Targeted redaction removes selected object-map entries, commits tombstones for their unshared
frames, rebuilds the index, and compacts only affected packs. It is separate from lease-driven
reachability collection so unrelated repository objects remain available.

Build-cache collection roots are canonical `action-index-root` records plus named result leases
under the same store. The reference registry traverses root to verification attestations and traces,
result to action/output/output-tree/stdout/stderr/provenance/SBOM attestations, ABI descriptors,
and ABI compatibility attestations, each `sbom/1` attestation to its canonical SBOM record, and
action to input/toolchain/sandbox. The index root retains verification
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
through the public store API. A shared-store repository writes its checkpoint, journal, session and
replication-cursor entry points as `repositories/<id-hash>/` keys through the store's own backend
(liquid #326) and creates repository-identified lease owners while all immutable objects remain in
the common packs and index. `put_if_absent` is the conditional create its exclusive create needs:
the disk backend hard-links a call-unique `.backend-tmp` temporary to the key, and the base class
refuses with `errc::unsupported`. The disk `list` walks only the prefix's directory.

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

`owner_lock.cpp` is the cross-process owner lock taken by `store_options::exclusive_owner` (liquid
#321): an exclusive close-on-exec `flock` (POSIX) or `LockFileEx` (Windows) on `<root>/store.lock`,
taken in `store_backend::open` before the fixed-name temporary files are removed and released last
when the store is destroyed. A bounded `owner_wait` retries the lock with a doubling 5–100 ms delay;
expiry returns `errc::locked` with the holder's recorded pid, host and start time. See
`docs/library/store.md#process-ownership-and-hand-off`.

`store_impl.cpp` also holds the store side of liquid #328's whole-repository transfer:
`transfer_describe` (how a store holds an identity: object, chunk, registration), `transfer_copy_chunk`
and `transfer_copy_object` (a verified chunk at a time from the source store, appended under the same
digest; an object published only after its whole-content digest and chunk-list descriptor are
recomputed and matched), `transfer_copy_remote`, `transfer_publish` (index flush and object map
after a barrier, only when something was appended), and the `transfer_verify_*` reads. The walk,
the state keys and the lease records are in `src/repository/transfer.cpp`. See
`docs/library/store.md#whole-repository-transfer`.

`write_lock.cpp` is the cross-process write lock of a disk store opened without an owner (liquid
#526): an exclusive `flock` (POSIX) or `LockFileEx` (Windows) on `<root>/store.write.lock`, held
for one mutation. Its first eight bytes are a generation the holder raises when it changed the
store; `store_impl.cpp`'s `hold_writes` takes it under the mutation mutex, nests within one handle,
and reloads the handle's state (`refresh`, through the same `load_state` open uses) when another
process published. The disk backend counts its own mutations (`backend_mutations`) so a hold that
changed nothing leaves the generation alone. `store.cpp` retries a read that missed once after a
reload. See `docs/library/store.md#several-writers-without-an-owner`.

`holder.cpp` names this process as a lease holder (`holder <host> <pid> <start>`) and tells
whether a holder named in a lease owner is alive, dead or on another host (liquid #527).
`leases.cpp`'s `release_lease_now` releases a lease from a destructor: a disk lease's file is
removed at once, another backend's release is started on the runtime.

`owner.cpp` holds the injected owner of liquid #327: `owner_grant` (a fence and a shared lost
flag), `make_disk_store_owner` (the flock above as a `store_owner` with fence zero),
`owned_store_backend` (the wrapper around a backend's fenced view that refuses mutations, and reads
unless opted in, with `errc::locked` once the grant is lost, and revokes the grant when the view
refuses a stale fence), and `owner_hold` (what an open store keeps; it releases an injected owner
on `store::close()` or, when destroyed without it, on the runtime). `owned_store_backend` forwards
`put_if_absent` as a mutation; a new `store_backend` operation must be forwarded the same way. See
`docs/library/store.md#injected-owners-and-fencing`.

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
Conflict records are detected as their own canonical kind and retain the base, left, and right
objects. Trees and version-3 changes retain conflict digests; conflict traversal retains base,
left, right, and an optional suspicious-merge proposal; resolution changes retain both the prior
conflict and selected value.
