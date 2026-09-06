# Search

> Research: [19-full-text-search](../research/19-full-text-search/)
>
> **Requirement:** full-text search built in, and the ability to serve search.

**Implementation status:** liquid #22 and #67 implement repository-local working-tree and
named-reference search with content-addressed BM25F, verified positional trigrams, a parallel
current-blob scan, measured cost planning, atomic publication, daemon drip updates, public result
and plan APIs, and `liquid search --explain`. See
[`docs/library/search.md`](../library/search.md). Packaged-artifact search waits for the Phase 5
format registry; symbols remain #66.

## Three executors, one query planner

No single index is right for both prose and code. liquid runs two, plus a scan executor, and chooses
the cheapest executor with an identical result set from measured size, freshness, selectivity, and
machine calibration inputs.

| Engine | Handles | Why |
|---|---|---|
| **[`oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp) / BM25F** | Ranked full-text and relevance — documentation, prose, commit messages, and the consumer Drive content ([15](15-consumer-sync-and-web.md)) | Token-based ranked retrieval, described upstream as covering ~90% of real queries. Coroutine-native, `std::expected`-based, matching liquid's conventions ([17](17-implementation.md)). |
| **Positional trigram index** | Exact substring and regex over **code** | Token analysis mishandles what matters in source: punctuation, partial identifiers, `snake_case`/`camelCase` boundaries, operators. This is the Zoekt/Blackbird tradition. |
| **Brute-force scan** | Working trees changing faster than any index can follow | ripgrep-class scanning is genuinely fastest below a threshold — no ingest delay, no staleness. |

Being explicit: **BM25F is not a substring or regex engine.** That is not a criticism
of oramacore-cpp, which does exactly what it is designed for — it means liquid needs
the trigram index too, and the two are complementary rather than redundant.

## Indexing a content-addressed corpus

liquid's advantage is structural. The store is already content-addressed, so:

```
Blob(blob_hash, language, index_doc_id)
Occurrence(repo, tree_or_snapshot, path, blob_hash)
Ref(repo, name, change_hash, tree)
```

The inverted index returns **blob hashes**; a second stage joins them to occurrences
reachable from the requested ref and emits paths.

Identical content across branches, forks, versions, and even inside different package
formats is **indexed once**. This is GitHub Blackbird's design — which reduced 115 TB
across 45M repositories to ~28 TB unique content — except it is native to liquid
rather than reconstructed.

The hard part is occurrence metadata, not content indexing: one blob may appear at
thousands of paths; deleting a branch does not free a blob still reachable elsewhere;
and materialising every change's full tree recreates enormous duplication. liquid
shares unchanged subtrees through the same Merkle structure the store already uses.

## Incremental by default

**Sourcegraph's default is a complete repository reindex on change**, even for
multi-gigabyte monorepos; its delta indexing was validated but never shipped as the
default. That is the same "build the world" pathology as
[04](04-build-engine.md), and liquid has no excuse for it — it knows exactly which
blobs changed.

- Changed blobs produce delta segments; replaced occurrences get tombstones.
- Compaction runs on a bounded schedule with backpressure, so a commit storm cannot
  produce thousands of tiny segments.
- Patches from the datastore ([17](17-implementation.md)) drive invalidation
  directly — the same signal that drives incremental rebuilds.
- **Publication is change-atomic**: build all changed documents, then advance the ref
  pointer. A partially indexed state is never visible.

## Searching inside artifacts

Because liquid decomposes containers ([03](03-diff-and-merge.md)), it indexes
*inside* a `.deb`, a wheel, a jar or an OCI layer.

Searching the contents of published artifacts is a capability no existing code-search
engine offers, and it comes almost free from machinery liquid needs anyway.

## Symbols from the compiler

liquid's incremental graph already holds resolved symbol information
([04](04-build-engine.md)). Precise definitions and references — SCIP-equivalent —
are a **byproduct of building**, not a separate CI indexing job.

Unlike every existing system, they are fresh on every build rather than as stale as
the last nightly index run. tree-sitter provides the syntactic fallback for languages
liquid cannot compile.
That fallback consumes the same graph-owned canonical parse used by build units and structural
source diff, implemented by liquid #68.

## History

Hybrid, because indexing every change is prohibitive:

- **Hot index** on branch tips — freshest, smallest.
- **Cold snapshots** for releases and tags.
- **Scan** for arbitrary historical changes.

## Serving search

Search is served from the same store, with the same authentication and the same
transparency guarantees as the artifact service ([09](09-artifact-service.md)).
Permission filtering applies in every channel, checked at query time.

## Open questions

- **Trigram index storage cost.** Zoekt-style positional trigrams run roughly 3× the
  corpus. Acceptable, but it must be budgeted and sharded well below the 4 GiB
  `uint32` offset ceiling that constrains Zoekt.
- **Encrypted content** cannot be server-side indexed
  ([11](11-secrets-and-encryption.md)).
- **Whether vector/semantic search earns its cost** — 1M 768-dim float32 vectors is
  3 GB before the ANN graph, and embeddings are weak at exact identifiers.
