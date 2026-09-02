# Full-text and code search

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid has full-text search built in, and can *serve* search.
> (Steve already has the libraries.)

## The headline finding

For code, **a code-specific substring/regex index beats a word-oriented full-text
engine.** Lucene-style tokenisation mishandles exactly what matters in source:
punctuation, partial identifiers, `snake_case`/`camelCase` boundaries, operators
and generated strings. Zoekt, GitHub's Blackbird and Russ Cox's trigram design are
the right family.

And the baseline matters: **ripgrep is often fast enough.** No ingest delay, no
index storage, no staleness, and SIMD literal scanning gives roughly 0.6 GB/s/core
(GitHub measured 2.769 s over a cached 13 GB). Indexing only pays once query volume
× corpus size exceeds what brute force can absorb. GitHub's own number for when it
stops working: 96 s/query on 2,048 cores for a 115 TB corpus — 0.01 QPS.

## Trigram indexes, and the positional upgrade

**Russ Cox's design** (Google Code Search, `codesearch`): map trigram → sorted file
IDs. A regex is analysed into a boolean expression over mandatory trigrams; the
posting intersection produces *candidates*; the full regex then verifies each one.
Measured: a 77 MB index over 420 MB of Linux source (~20%), reducing 36,972 files
to 25 candidates and 1.96 s to 0.01 s. A pattern with no mandatory three-character
literal degrades to a full scan.

**Zoekt** upgrades this to **positional** trigrams: trigram → occurrence offsets.
For a literal it picks a *selective pair* of trigrams and verifies their required
distance, rejecting most false positives without reading file content. The cost is
size: roughly **3× the corpus** (≈1× content, ≈2× positions). Practical limits
worth knowing: `uint32` offsets cap a shard at <4 GiB and about 1 GiB of source
content, so large repositories are sharded.

**Zoekt's branch handling is the most directly useful idea for liquid.** Each
distinct `(path, content)` version is stored **once** and tagged with a **bitmask of
the branches containing it**. Identical files across `main`, `release` and
`staging` share content and postings. Capped at 64 indexed branches per repository.

**livegrep** uses suffix arrays instead — every substring is searchable without
choosing an n-gram size, at roughly 5× storage before dedup and expensive
construction. Better for static corpora.

## GitHub Blackbird: the scale reference

Published beta figures: ~45 million repositories, 115 TB, 15.5 billion documents,
reduced to ~28 TB unique content after deduplication, with a complete index of
**25 TB**. Ingest ~120,000 documents/s — 36 h for the full corpus, ~18 h for a
reindex after delta crawling. Per-shard p99 ~100 ms.

Two architectural decisions matter:

1. **Sparse, variable-length n-grams.** Rather than fixed trigrams, a
   weighted-bigram algorithm recursively selects *informative* grams down to
   trigrams, avoiding hyper-common ones like `for`. More selective, at the cost of
   requiring the identical tokeniser at ingest and query time.
2. **Shard by Git blob SHA.** Immutable content distributes evenly; an identical
   blob is indexed **once**; a separate metadata tree records which repositories,
   branches and paths it occurs in. This avoids hot shards from popular repos and
   forks. The cost: one repository's blobs span all shards, so every query fans out.

Publication is **commit-atomic**: build all changed documents for commit C, then
atomically advance branch metadata. Users never see a half-applied commit.

## Incremental indexing

The mechanics are LSM-shaped and well understood: buffer in RAM, flush **immutable
segments**, search all live segments in parallel, mark deletes with **tombstones**,
and reclaim space through **merges** whose write amplification is several times the
ingested bytes. Near-real-time visibility comes from publishing a new segment
manifest without waiting for a durable global commit — *visible* and *fsync-durable*
are different things. Backpressure is mandatory, or a commit storm produces
thousands of tiny segments and destroys tail latency.

**The cautionary tale:** Sourcegraph's default is still a **complete repository
reindex** on change, even for multi-gigabyte monorepos. Zoekt has optional delta
indexing (changed files form delta shards, old versions get tombstones, full
rebuild after ~150 deltas) — validated but **not shipped as the default**. So the
default rebuild cost is O(all indexed files), not O(diff), and a high-churn
monorepo starves the indexer and lags behind the tip.

That is precisely the "build the world" pathology from
[04-build-systems](../04-build-systems/), appearing in a search index. liquid
should not repeat it.

## Indexing a versioned, content-addressed corpus

The right schema separates immutable content from mutable placement:

```
Blob(blob_oid, bytes, language, lexical_index_docid)
Occurrence(repo_id, tree_or_snapshot_id, path_id, blob_oid)
Ref(repo_id, ref_name, commit_oid, tree_or_snapshot_id)
```

The inverted index returns **blob IDs**; a second stage joins them to occurrences
reachable from the requested ref and emits paths. Identical blobs are indexed once.

**The hard part is occurrence metadata, not content indexing.** One blob may appear
at thousands of paths across repositories and commits; deleting a branch does not
free a blob still reachable elsewhere; "last modified" is not intrinsic to a blob
and must be derived; and materialising every commit's full tree recreates enormous
duplication.

Granularity options: per-commit (exact, prohibitive), per-branch-tip (the usual
default, freshest, loses history), multi-branch-tip with bitmaps (Zoekt's), periodic
snapshot (predictable retention), or **hybrid** — hot index on branch tips, cold
snapshots for releases, `git grep`-equivalent for arbitrary commits. Hybrid is the
recommended repository-scale design.

## Symbols and semantics

- **Universal Ctags** — fast, broad, heuristic. Good for "find the declaration
  named X".
- **tree-sitter tags** — AST captures (`@definition.function`, `@reference.call`).
  Structured and incrementally parseable. Sourcegraph uses tree-sitter for common
  languages with Ctags as fallback; indexed symbols added roughly 10% disk and
  memory.
- **SCIP** (superseding LSIF) — compiler-precise definitions, references and
  implementations, but requires a real build per commit and configuration.
  `scip-clang` indexes are 10–20% the size of equivalent LSIF.
- **Embeddings** — good for conceptual recall, bad at exact identifiers,
  punctuation and negation. Cost is concrete: 1M 768-dim float32 vectors = 3.072 GB
  before the ANN graph.

The consensus is **hybrid retrieval**: lexical for exact matches, SCIP for identity,
vectors for conceptual recall, with calibrated scoring and permission filtering in
every channel.

## Implications for liquid

1. **Index blobs and chunks once; keep occurrence metadata separate.** This is
   Blackbird's design and it is *native* to liquid, because liquid's store is
   already content-addressed. Identical content across branches, forks, versions and
   even inside different package formats is indexed once. That is a structural
   advantage over systems that bolt search onto a filesystem.

2. **Delta indexing as the default, not an unshipped option.** Sourcegraph's
   full-reindex default is the mistake to avoid, and liquid has no excuse for it:
   it already knows exactly which blobs changed. Index changed blobs, tombstone
   replaced occurrences, compact on a bounded schedule.

3. **Commit-atomic publication.** Build all changed documents, then advance the ref
   pointer atomically. Never expose a partially indexed commit.

4. **Positional trigrams for code, and budget ~3× storage.** Accept the cost; the
   selectivity is worth it. Shard well below the 4 GiB/uint32 ceiling that
   constrains Zoekt.

5. **Reuse the format registry.** liquid decomposes archives, packages and images
   ([03](../03-structural-diff-and-formats/),
   [15](../15-binary-introspection/)), so it can index *inside* a `.deb`, a wheel,
   an OCI layer or a jar. Searching the contents of published artifacts is a
   capability no existing code-search engine offers, and it comes almost free.

6. **Symbols from the compiler, not from ctags.** liquid's incremental graph
   ([08](../08-incremental-compilation/)) already holds resolved symbol
   information. Emitting SCIP-equivalent precise references is a byproduct rather
   than a separate indexing job — and unlike everyone else, liquid gets it fresh on
   every build instead of from a periodic CI task.

7. **Serve search as a first-class service.** Same store, same auth, same
   transparency guarantees as the artifact service
   ([12](../12-artifact-distribution/)). Steve has the libraries; the architectural
   work is the occurrence model and the incremental pipeline, not the inverted
   index itself.

8. **Keep ripgrep-equivalent brute force in the product.** For a working tree
   changing faster than any index can follow, scanning is the correct answer. The
   query planner should choose between index and scan by cost, not by dogma.
