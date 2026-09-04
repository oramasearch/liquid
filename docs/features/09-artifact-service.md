# Artifact service

> Research: [12-artifact-distribution](../research/12-artifact-distribution/)
>
> **Requirement:** liquid is an artifact service that serves builds.

## One store, many views

Underneath: the content-defined chunk store ([01](01-object-model-and-storage.md)).

On top: **views** that map names and versions onto content hashes, speaking the
protocols clients already use — OCI, npm, PyPI, Maven, apt, generic.

This is what fixes the deduplication complaint about every existing registry. Because
liquid decomposes containers before storing them
([03](03-diff-and-merge.md)), the same vendored zlib inside a wheel, a `.deb` and an
OCI layer is stored **once** — not three times under three names in three
format-specific repositories.

## Protocols

- **OCI distribution** as the primary wire protocol. Universally deployed,
  CDN-friendly, and OCI 1.1's `subject`/Referrers gives attestation attachment for
  free.
- **REAPI CAS** for build traffic ([06](06-distributed-execution.md)).
- **Ecosystem views** for npm, PyPI, Maven, apt, and generic artifacts.
- **Model and dataset views** — the Hugging Face surface
  ([18](18-lake-service.md)). HF's own Xet migration validated content-defined chunking
  for checkpoints and datasets; liquid uses the same primitive and attaches training
  provenance to it.

No third protocol unless measurement forces one.

## Serving chunks

Transfer is by **packed block with HTTP range requests**, so a client fetches only
the byte ranges holding chunks it lacks. The dedup unit (chunks) and the transfer
unit (packs) stay distinct — a naive per-chunk object store dies of request overhead.

Lazy fetch is the default posture: clients receive manifests and pull content on
access, whether that content is a source tree, a build output, a package or a
container filesystem.

Delta encoding is an optimisation layered on top, not a replacement. `zstd
--patch-from` where a known base exists; executable-aware delta reserved for the
generate-once/apply-many cases — published releases and OTA-style updates — where
the asymmetric cost pays for itself. The implemented delivery path indexes signed `delta/1`
attestations by target digest, reads a matching patch through the local store (including a
registered remote blob), and falls back to ordinary chunks when none exists. Both patch apply and
chunk fallback verify the complete target digest before publishing bytes. The executable-aware
algorithm id is reserved but not emitted because the measured Mach-O section candidate did not
beat the generic patch; the measurements and threshold are in
[distribution.md](../library/distribution.md#recorded-savings).

## Transparency log

Every published artifact is recorded in an **append-only Merkle log**, following the
Go checksum database, which is the best-deployed example of this idea working.

Consequences: a client can verify it sees the same artifact everyone else sees; a
maintainer can detect an unauthorised publication; and "serve builds" becomes
trustworthy rather than merely convenient ([10](10-supply-chain.md)).

## Debug symbols by build-id

liquid runs a **debuginfod-compatible** endpoint. It already stores every
intermediate, including debug info, addressed by content — so serving symbols by
GNU build-id is nearly free.

The consequence is significant operationally: ship stripped binaries *and* keep full
symbols, without the usual tradeoff. A stripped production binary becomes fully
debuggable on demand, and resolvable back to its exact source and build.

## Retention

The universal complaint about existing registries is that retention policy is
incomprehensible and dangerous.

liquid expresses policy in terms users think in — "keep every release tag forever;
keep CI artifacts 30 days; keep anything referenced by a retained artifact" — and
computes reachability itself.

## Garbage collection

Designed before launch, because it cannot be retrofitted
([01](01-object-model-and-storage.md)):

- Leased roots with expiry, so an abandoned client does not pin storage forever.
- Epoch-based marking rather than global stop-the-world.
- Action Cache entries and their chunks share a lifetime.
- Chunk-level reference tracking cheap enough for billions of objects.

Getting this wrong produces either unbounded cost or corrupted artifacts, and both
are fatal for an artifact service.

## Search over what it holds

Because liquid indexes by chunk and blob, searching published artifacts — including
*inside* a `.deb`, a wheel or an OCI layer — is the same infrastructure, not a
bolt-on ([12](12-search.md)). No existing code-search engine offers this.

## Open questions

- **Multi-region replication** and how the transparency log stays consistent across
  regions.
- **Whether ecosystem views are read-through caches or authoritative registries**,
  which has very different trust and availability implications.
- **Anonymous public read** versus authenticated access, and how that interacts with
  the transparency log.
