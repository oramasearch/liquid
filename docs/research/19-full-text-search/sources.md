<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Full-text and code search at repository scale, 2024-2026

## Scope and bottom line

At repository scale, **lexical code search is still best served by a code-specific substring/regex index** - Zoekt, Blackbird, or a Cox-style trigram index - not by a word-oriented full-text engine. **ripgrep remains the correct baseline** for small or freshly changing scopes. For history, index **Git blob IDs once** and keep repository/tree/path/branch occurrence metadata separately. Add **SCIP/compiler indexes** for exact references and **embeddings only as a hybrid recall channel**, not as a replacement for lexical or symbol search.

## 1. Repository-scale trigram and suffix-array code search

| Engine / approach | Index and query design | Scale, updates and practical tradeoff |
|---|---|---|
| **Russ Cox / Google Code Search / `codesearch`** | Indexes `trigram -> sorted file IDs`, not positions. Regex analysis derives necessary Boolean trigram clauses from exact strings, prefixes, suffixes and alternations; e.g. `Google.*Search` becomes an AND of the literals' trigrams. The posting result is only a candidate set, so the complete RE2-style regex is run on each candidate. Patterns with no mandatory three-character literal degrade to `ANY` and scan everything. The index is mmap-backed and includes paths, files and posting lists. | The standalone `cindex`/`csearch` implementation produced a **77 MB index for 420 MB** of Linux 3.1.3 source - about **20%**. For `hello world`, it reduced **36,972 files to 25 candidates** and about **1.96 s to 0.01 s**; case-insensitive expansion yielded 599 candidates. Refreshing rescans indexed roots and rewrites the index, so it is not a fine-grained mutable design. [web:16] |
| **Zoekt / Sourcegraph engine** | Uses **positional Unicode trigrams**: `trigram -> occurrence offsets`, for both content and filenames. For a literal, the planner chooses a selective pair - typically boundary trigrams, but it may choose a rarer interior pair - and verifies their required distance. Regexes are reduced to mandatory substring expressions, evaluated as Boolean query trees, then verified with the full regex. Per-shard partial evaluation can turn `repo:` or `branch:` constraints into `FALSE` and skip a shard. | Faster phrase/substrings than file-level trigrams but much larger: approximately **3x corpus** - about 1x stored content and 2x positions. An mmap-able `.zoekt` shard holds contents, filenames, varint posting lists, filename postings, branch masks and metadata. `uint32` offsets impose a **<4 GiB shard** and roughly **1 GiB source-content** ceiling; large repositories are split because one goroutine searches a shard. [web:6][web:376] |
| **Zoekt branch representation** | Each distinct `(path, content)` version is stored once inside the repository index and tagged with a bitmask of branches containing it. Thus identical files on `main`, `release` and `staging` share content/postings, while divergent versions get separate records. | Supports up to **64 indexed branches per repository** and gives low overhead when branches are similar; it is not an all-commit historical index. [web:361][web:366] |
| **Hound** | A Go backend and React frontend built directly on Cox's trigram/file-candidate design. Searches repositories in parallel, intersects necessary file-level trigram lists, and scans candidates with the complete regex. | One index per repository; it polls for changes every **30 seconds** by default. On revision change it builds a new index and atomically swaps it, meaning a large changing repository incurs a full rebuild rather than cheap per-file mutation. [web:316][web:318] |
| **livegrep** | C++/pthread server using RE2 over **suffix arrays**, built with `libdivsufsort`. Source is flattened into large buffers; identical lines are deduplicated and a content map maps offsets back to files. Literals or character ranges define suffix-array intervals; chained searches refine regex prefixes, and RE2 verifies final matches. | Every substring is implicitly searchable without selecting a fixed n-gram. A basic 32-bit suffix array costs roughly 4 bytes per input byte, plus original text and maps - around **5x before deduplication/overhead** - and rebuilding/sorting is expensive. Best for static, roughly gigabyte-scale corpora. [web:331][web:333][web:341] |
| **ripgrep brute-force baseline** | No persistent index. Parallel directory walking; skips hidden, binary and Git-ignored files; Rust finite-automata regex engine, aggressive mandatory-literal extraction and SIMD literal scanning; streams many-file workloads and may mmap suitable single files. | Often wins below repository/few-GB scope because there is no ingest delay, index storage, stale state or reindex cost, and a warm page cache plus SIMD can consume source very quickly. GitHub measured **2.769 s for a cached 13 GB file**, about **0.6 GB/s/core**. It becomes untenable when every global query must scan tens or hundreds of terabytes: GitHub estimated **96 s/query on 2,048 cores**, or **0.01 QPS**, for its 115 TB corpus. [web:46][web:346][web:353] |

### Why positional trigrams matter

Cox's index answers, "Which files contain all mandatory grams?" Zoekt answers, "At which offsets do selective grams occur, and are those offsets the required distance apart?" The latter touches fewer posting lists and rejects many false positives before reading file content, at the price of roughly an order-of-magnitude larger index than `codesearch`'s file-level postings.

For regexes such as `(Path|PathFragment).*=.*/usr/local`, Zoekt first evaluates something equivalent to `AND(OR("Path", "PathFragment"), "/usr/local")` and only then executes the complete regex. Regexes such as `.{3}`, `a?`, or broad character classes with no required three-rune literal still approach a full scan. [web:6]

---

## 2. Sourcegraph and GitHub Blackbird at scale

| System / component | Architecture | Updates, costs and published scale |
|---|---|---|
| **Sourcegraph query routing** | The frontend determines which `repository@revision` pairs are present in Zoekt. It concurrently sends indexed pairs to Zoekt and missing revisions to **Searcher**. Searcher is horizontally scalable and stateless: for each repository request it fetches a commit archive from gitserver and scans its files. Non-default branches and arbitrary commits remain searchable but cost approximately a parallel `git grep`/archive scan per repository. [web:70] | Default-branch/global search is optimized; arbitrary-revision search trades storage for latency. |
| **`zoekt-webserver`** | Watches its index directory, mmap-loads/unloads `.zoekt` shards, executes query trees over them and streams matches. Large repositories use multiple shards for parallelism. | Zoekt's design estimate is around **3x source on SSD** and at least **1.2x corpus RAM/page-cache capacity**; Sourcegraph's newer deployment guidance has recommended webserver memory near **2.7x non-binary source**. [web:376][web:381] |
| **`zoekt-sourcegraph-indexserver`** | Polls Sourcegraph's desired repository, commit, branch and indexing configuration - historically at most once per minute - compares it with on-disk metadata, fetches Git data and invokes `zoekt-git-index` to produce shards. [web:2][web:4] | Indexing and serving are separated so old shards remain queryable until replacements are ready. This implies temporary old-plus-new disk occupancy during rebuilds. |
| **Indexed symbols** | Sourcegraph originally used Universal Ctags during Zoekt indexing; current Zoekt extracts common-language symbols with **tree-sitter queries** and uses Universal Ctags for the long tail. Symbol declarations are stored as another searchable field. [web:61][web:75] | Fast `type:symbol` search, but syntactic - not compiler-resolved references, overloads or dynamic dispatch. Indexed symbol support was reported to add roughly **10% disk and memory** when introduced. [web:71] |
| **Changing Sourcegraph monorepo** | Sourcegraph's default remains a complete repository reindex, including for multi-gigabyte monorepos. Zoekt contains optional delta indexing: changed files form delta shards and old versions receive file tombstones; after a configurable threshold, conventionally **150 deltas**, a full rebuild occurs. Sourcegraph states this was validated but not shipped as its default. [web:391][web:392] | Default rebuild work is therefore approximately **O(all indexed files)** for an updated monorepo, not O(diff). A high-churn monorepo can consume a disproportionate share of indexer CPU/I/O and exhibit tip-to-index lag. Delta mode reduces foreground work to O(changed bytes) but adds shard fan-out, tombstones, eventual compaction, and fallback rebuilds. |
| **GitHub Blackbird** | Custom Rust engine. Uses sparse, variable-length n-grams for content, path and symbols, plus posting indexes for language, owner and repository. Sorted-posting iterators are intersected/unioned lazily; only enough results are read to produce the requested top hits. A coordinator rewrites permissions/scopes, fans every global query to all blob-ID shards, merges scores, rechecks ACLs and returns the top 100. [web:46] | Published beta corpus: **almost 45 million repositories, 115 TB and 15.5 billion documents**. Deduplication/delta processing reduced this to about **28 TB unique content**; the complete index, including compressed unique source, was **25 TB**. [web:46] |
| **Blackbird sparse grams** | Rather than indexing only trigrams, a weighted-bigram algorithm recursively selects informative variable-length grams down to trigrams. The query uses a covering set of the same grams, avoiding very common grams such as `for`. For `/arguments?/`, the illustrated plan includes `arg`, `rgu`, `gum`, then either `ume` + `ment` or `uments`. [web:46] | More selective than fixed trigrams but requires the same deterministic tokenizer at ingest and query time. Exact content verification remains mandatory. |
| **Blackbird sharding and deduplication** | Shards by **Git blob SHA**, evenly distributing immutable content and indexing an identical blob only once. A separate metadata tree records the repositories, branches and paths in which the blob occurs. Because one repository's blobs span shards, global and repository queries fan out rather than enjoying repository locality. [web:272] | Avoids hot shards caused by famous repositories/forks and permits independent storage/read scaling. Metadata for popular blobs may still be large. |
| **Blackbird indexing and deletion** | Push events enter Kafka; crawlers diff Git state, symbol extraction runs, and `(blob, occurrence metadata)` updates are partitioned to index shards. New small indexes are published and compacted into larger ones; compaction k-merges postings in rank order and removes deleted documents. | Published ingest was approximately **120,000 documents/s**: **36 h** for 15.5 billion documents, or about **18 h** for a full reindex after delta crawling eliminated over half the work. Individual-shard **p99 was about 100 ms**; a 64-core host had a stated upper bound around **640 QPS**. Commit updates become visible atomically at commit granularity. [web:46] |

---

## 3. General full-text engines usable from a C++ system

These engines are primarily **token/term indexes**. Exact arbitrary code substring and regex search requires a character n-gram analyzer or a separate code index; tokenization alone mishandles punctuation, partial identifiers and generated strings.

| Engine | Integration and index model | Update latency, footprint and best fit |
|---|---|---|
| **Apache Lucene** | Java library; call through JNI or serve with Solr/custom Java. Extremely mature analyzers, BM25, positions, automata, facets and vector search. An index is a set of immutable, independently searchable segments. | Adds/updates flush new segments; update is add-new plus delete-old. A refreshed reader can expose uncommitted segments in milliseconds; tiered merges reclaim tombstones. Strongest general-purpose option, but JVM and merge write amplification matter. [web:107][web:117] |
| **Tantivy** | Rust Lucene-like library, embeddable through a Rust/C++ FFI boundary or served. mmap-oriented immutable segments, term dictionary, postings, BM25 and facets; it underlies Quickwit. | Documents become searchable after commit/reload; `LogMergePolicy` merges similarly sized segments and removes tombstones. The writer's minimum memory budget is approximately **15 MB per indexing worker**. Tantivy 0.22 reported **~40% higher GitHub-dataset indexing throughput** and HDFS-test indexing memory falling from **760 to 590 MB**. [web:121][web:126][web:133] |
| **Xapian** | Native C++ library - the cleanest direct C++ embedding option. Its Glass backend uses compressed postings and B-tree storage; supports ranked term search, positions, transactions and database/shard composition. | `WritableDatabase` supports add/replace/delete and atomic commit, but batching as many changes as memory permits is strongly recommended. Smaller operational footprint than Lucene; fewer modern distributed, vector and analytics facilities. [web:211][web:213][web:215] |
| **SQLite FTS5** | Native SQLite C API; virtual table with configurable tokenizers, prefix indexes, BM25 and external/contentless table modes. Ideal for a desktop tool, single repository or local cache. | Transactions make updates immediately manageable, but SQLite's writer serialization limits sustained parallel ingest. FTS5 builds multiple B-tree segments and incrementally merges them; `optimize` creates one compact B-tree. Low process overhead, but full stored content plus the FTS index can duplicate source unless external-content mode is used. [web:91] |
| **PostgreSQL `tsvector` + GIN** | Server accessed from C++; excellent if repository metadata already lives in PostgreSQL. Use `pg_trgm` rather than linguistic FTS for arbitrary identifier substring matching. | MVCC gives transactional visibility. GIN `fastupdate` batches writes in an unsorted pending list, reducing immediate write cost but causing periodic cleanup spikes; autovacuum, WAL, table versions and GIN bloat raise storage/write amplification. [web:93][web:103] |
| **Bleve** | Go library, so serve it or bridge via C ABI. The Scorch backend is a segmented index; all segments can be searched concurrently. Supports analyzers, BM25, facets and FAISS-backed vectors. | Immutable segments simplify concurrent reads but require background merges and tombstone reclamation. Less attractive than Xapian/SQLite for direct C++ linkage. [web:166][web:168] |
| **Vespa** | Distributed serving platform with Java configuration and a native C++ data plane. Combines text, structured fields, ranking tensors and vectors. Recent indexed text enters a mutable in-memory B-tree; immutable disk indexes are subsequently flushed and fused. | Text changes are searchable when acknowledged. In-memory attributes can be updated in place and sustain **tens of thousands of updates/s/node**; indexed-field partial updates require document-store read-modify-write. Excellent for large, continuously updated ranked/hybrid retrieval, but operationally heavier than a library. [web:169][web:174][web:179] |
| **Meilisearch** | Rust HTTP server built around memory-mapped LMDB. Typo tolerance, filters, ranking and vectors; oriented toward application/site search rather than regex code search. | Writes and setting changes are FIFO asynchronous tasks. Default maximum indexing RAM is **two-thirds of available RAM**. Official guidance suggests starting with disk around **10x raw data**. [web:136][web:139][web:142][web:197] |
| **Typesense** | C++ server, but not generally consumed as an in-process library. Keeps the complete search index in RAM and raw documents on disk; supports typo, facets and HNSW vectors. | Very low query latency and immediate updates, but RAM is the scaling dimension: official guidance is **2-3x the indexed/searchable source fields**. Published tests used **900 MB RAM for 2.2M recipes**, indexing in **3.6 min** and serving 104 concurrent queries/s at 11 ms average on 4 vCPUs; 28M books used 14 GB and indexed in 78 min. [web:147][web:181][web:185] |
| **Quickwit** | Rust/Tantivy distributed service designed around cheap object storage. Indexers produce immutable Tantivy files called **splits**, upload them to S3/GCS/Azure, and publish metadata; stateless searchers fetch needed split ranges and use local hot caches. [web:151][web:152] | Best for append-heavy logs or snapshot corpora, not frequent point mutation. A split is emitted after a target document count or `commit_timeout_secs`. Object storage gives cheap, elastic capacity but adds publication latency and cold-read variance. Published tests reported **27 MB/s indexing**, subsecond basic queries on 20M logs, and **$2 per ingested TB**. [web:155][web:158][web:163] |

### Selection for a C++ implementation

| Requirement | Recommended starting point |
|---|---|
| In-process, small/local corpus | **SQLite FTS5** |
| In-process native ranked text search | **Xapian** |
| Rich general-purpose text ranking and analyzers | **Lucene service** or **Tantivy sidecar/FFI** |
| Exact repository substring/regex | **Zoekt service**, or a purpose-built positional trigram index |
| Entire hot index in RAM with simple API | **Typesense** |
| Continuously updated distributed hybrid retrieval | **Vespa** |
| Very large append-only, object-storage-resident corpus | **Quickwit** |
| Website/product search with typo tolerance | **Meilisearch** |
| Small corpus changing faster than indexing can follow | **ripgrep** |

---

## 4. Incremental inverted indexing

| Mechanism | Technical operation | Cost / failure mode |
|---|---|---|
| **Immutable segments** | Buffer documents and postings in RAM; flush a complete small segment. Search all live segments in parallel and merge results. | Refresh is cheap and readers remain lock-free, but segment count grows and each query acquires more dictionaries/posting iterators. Lucene and Tantivy use this model. [web:117][web:121] |
| **Update / replacement** | Maintain a stable logical key such as `(repo, branch-or-snapshot, path)`. Mark the previous internal document ID deleted and append the new document to a fresh segment. | Avoids random rewriting of compressed postings, but temporarily stores both versions and increases delete bitsets. |
| **Tombstones** | Record deletes in a per-segment live-document bitmap or a newer delta layer. Search filters dead IDs; compaction omits them. | Fast foreground deletes; disk is not reclaimed until merge. Too many tombstones waste I/O and distort ranking/statistics. |
| **Near-real-time visibility** | Publish a new segment manifest/reader without waiting for a durable global commit. A durable WAL or commit point protects recovery. | Millisecond-to-second freshness is possible, but "visible" and "fsync-durable" are distinct. [web:107][web:109] |
| **Merge policy / LSM behavior** | Size-tiered or leveled policies combine similarly sized segments, k-way merge term dictionaries and postings, renumber live documents, then atomically replace the manifest. | Amortized ingestion is good, but total write amplification can be several times ingested bytes. Larger merges improve query locality but consume CPU, disk bandwidth and temporary old-plus-new space. |
| **Backpressure** | Bound RAM buffers, outstanding segments and merge debt; throttle ingestion when compaction falls behind. | Without it, a commit storm creates hundreds of tiny segments, raises query tail latency and eventually exhausts disk. |
| **Branch-consistent publication** | Build all changed documents for commit `C`, then atomically advance branch metadata from `C-1` to `C`. | Users see either complete old or complete new branch state, never a partial commit - the consistency behavior Blackbird describes. [web:46] |

### Version-control indexing granularity

| Granularity | Representation | Advantages | Costs / recommendation |
|---|---|---|---|
| **Per commit** | Every commit is a logical snapshot, usually with blob deduplication and commit/path occurrence tables. | Exact reproducibility and arbitrary historical search. | Metadata/reachability grows with `commits x changed paths`; indexing each whole tree is prohibitive. Reserve for regulated or high-value history, and index only changed blobs. |
| **Per branch tip** | One logical document per current branch path; each push tombstones changed paths and adds replacements. | Low metadata and best freshness for normal developer search. | Force-pushes and deleted branches require reachability cleanup; old commit search falls back to Git scanning. This is the usual default. |
| **Multi-branch tip** | Index distinct blob/path versions once and attach a branch bitmap or branch-set ID. | Efficient when release branches differ little; Zoekt's model. | Branch masks are bounded and branch movement still changes metadata. |
| **Periodic snapshot** | Immutable daily/release/tag segments plus a mutable current-tip delta. | Predictable retention, cheap rollback and object-storage compatibility. | Historical results are only exact at snapshot boundaries unless combined with commit deltas. Often the best compromise for long-lived history. |
| **Hybrid** | Current branch tips in a low-latency index; release/tag snapshots in cold segments; arbitrary commits via `git grep`. | Controls hot-index size while retaining complete functional access. | Multiple execution paths and potentially different latencies. This is the recommended repository-scale design. |

---

## 5. Branches, history and content-addressed indexing

| Search task | Correct mechanism |
|---|---|
| Current worktree/index or a named commit/tree | `git grep pattern [<tree>...]`; Git can search tracked worktree files, index blobs or supplied tree objects. [web:241] |
| All reachable commits | `git rev-list --all \| xargs git grep pattern`; exact but repeatedly traverses many overlapping trees. |
| Find where a literal's occurrence count changed | `git log -S<string> -p --all`; `-S` compares the count of a string between parent and child. Add `--pickaxe-regex` for extended regex. |
| Find diffs with added/removed lines matching a regex | `git log -G<regex> -p --all`; `-G` searches patch text rather than count changes. `--pickaxe-all` returns every file change in qualifying commits. [web:243] |
| Search indexed branch tips | Filter a Zoekt/Blackbird-style occurrence map by repository and branch, then search blob postings. |
| Search exact historical state interactively | Resolve commit -> tree -> `(path, blob ID)` set; execute the text query against unique blobs, then intersect results with that tree's occurrence set. |

A history-capable content-addressed schema should separate immutable content from mutable placement:

```text
Blob(blob_oid, bytes, language, lexical_index_docid)
Occurrence(repo_id, tree_or_snapshot_id, path_id, blob_oid)
Ref(repo_id, ref_name, commit_oid, tree_or_snapshot_id)
Commit(commit_oid, parents[], tree_oid, time)
```

The inverted index returns **blob IDs**. A second stage joins those IDs to occurrences reachable from the requested repository/ref/commit and emits paths. Since identical Git blobs have the same object ID, forks and repeated files are lexically indexed once; GitHub Blackbird explicitly shards by blob SHA for both deduplication and even distribution. [web:272]

The hard part is not content indexing but occurrence metadata:

* The same blob may occur at thousands of paths, repositories and commits.
* Deleting a branch does not mean deleting a blob if another reachable ref still uses it.
* "Last modified" is not intrinsic to a Git blob; it must be derived from commit/path history.
* Materializing every commit's full tree recreates enormous duplication.

Use persistent/Merkle tree nodes, branch-tip bitmaps, path-change intervals, or snapshot-plus-delta manifests so unchanged subtrees are shared. Garbage-collect lexical blobs only when no retained snapshot/ref/commit occurrence references them.

---

## 6. Symbol, precise-semantic and embedding search

| Technique | What is indexed | Precision, freshness and tradeoff |
|---|---|---|
| **Universal Ctags** | Declaration name, kind, language, path and location, extracted heuristically from syntax. | Fast, broad language coverage, cheap enough to fold into lexical indexing. Good for "find declaration named X"; cannot reliably resolve scopes, overloads, imports or references. |
| **tree-sitter tags** | Parser AST captures such as `@definition.function`, `@reference.call` and inner `@name`; optional `@doc`. [web:256][web:262] | More structured and incrementally parseable than regex-based Ctags. Still syntax-level: no type checking, build flags, generated code or cross-package resolution. Sourcegraph uses tree-sitter for common languages and Ctags as fallback. [web:260] |
| **LSIF** | Language-server-produced graph of ranges, definitions, references, hovers, result sets and cross-package monikers. | Compiler/language-server precision but verbose and costly to generate/process. Mostly superseded by SCIP in Sourcegraph. |
| **SCIP** | Language-agnostic Protobuf with documents, occurrences, relationships and human-readable symbol IDs. Supports go-to-definition, references and implementations. [web:226][web:227] | Exactness depends on a real build/compiler indexer. Requires one index per relevant commit/configuration and must be regenerated when source, compiler flags or dependencies change. For C/C++, Sourcegraph reported `scip-clang` indexes at only **10-20% of corresponding LSIF size**. [web:238] |
| **Search-based code intelligence** | Lexical symbol declaration index plus text references and language heuristics. | Always available and comparatively fresh but can confuse same-named symbols. Useful fallback when no SCIP artifact exists. |
| **Embedding / ANN code search** | Function-, class-, file- or chunk-level vectors; ANN commonly uses HNSW. Retrieves by conceptual similarity to natural-language or example-code queries. | Finds "retry with exponential backoff" when those words are absent, but exact identifiers, punctuation, negation, rare APIs and long mixed files are problematic. Model inference adds ingest/query latency and every changed chunk must be re-embedded. |
| **Hybrid retrieval** | Union lexical BM25/trigram, symbol/SCIP and vector candidates; rerank with structural signals or a cross-encoder. | Usually the robust production design: lexical search preserves exact matches, SCIP supplies identity, and vectors improve conceptual recall. Requires score calibration and permission filtering in every channel. |

### Vector cost in concrete terms

A raw float32 embedding costs `documents x dimensions x 4 bytes`. Thus **1 million 768-dimensional chunks consume 3.072 GB before ANN graph, IDs, metadata, replicas or allocator overhead**. With HNSW `M=16`, the lowest-layer neighbor IDs add roughly another 128 MB if stored as two sets of 16 32-bit links, while upper layers and implementation overhead add more. Typesense's HNSW defaults are `M=16` and `ef_construction=200`. [web:292]

Chunking is equally important:

* **Function/class chunks** give interpretable results and manageable re-embedding.
* **File vectors** dilute unrelated concepts in long files.
* **Tiny sliding windows** improve local recall but multiply vector count and return redundant snippets.
* Embeddings do not encode repository reachability, ACLs or symbol identity; those remain lexical/metadata filters.
* Public code-retrieval scores are highly dataset-specific: CodeSearchNet, for example, evaluates with NDCG, so leaderboard performance should not be read as repository-level exact-search recall. [web:311]

## Recommended architecture

| Layer | Recommended design |
|---|---|
| Fresh working branch | ripgrep fallback plus a current-tip Zoekt-style positional-trigram index |
| Publication unit | Commit-atomic branch manifest |
| Updates | Changed-blob delta segments, path tombstones, bounded merge policy and periodic full compaction |
| Storage key | Git blob OID; separate repository/tree/path/ref occurrence metadata |
| History | Indexed release/daily snapshots plus arbitrary-commit `git grep`; only index every commit when required |
| Symbols | tree-sitter/Ctags in the lexical index |
| Exact navigation | CI-generated SCIP indexes keyed by commit and build configuration |
| Conceptual search | Chunked code embeddings as a hybrid candidate source |
| Large cold corpus | Immutable snapshot/split files on object storage, Quickwit-style, with hot local caches |
| Operational trigger | Use brute-force below the point where an indexed query's saved scan CPU exceeds index build, storage, merge and staleness costs |

The central design tension is **query amplification versus write amplification**: ripgrep pays for every query; Cox indexes pay modest storage but still verify files; Zoekt pays roughly 3x storage for positional selectivity; suffix arrays pay still more build/RAM; segment-based general indexes pay continuous merge cost; and historical or semantic systems add occurrence metadata, compiler work or vectors that can exceed the lexical index itself.

## Citations

[2] https://github.com/nmpowell/sourcegraph/blob/main/doc/dev/background-information/architecture/life-of-a-search-query.md
[4] https://github.com/sourcegraph/sourcegraph-public-snapshot/blob/main/doc/dev/background-information/architecture/life-of-a-search-query.md
[6] https://github.com/sourcegraph/zoekt/blob/main/doc/design.md
[16] https://swtch.com/~rsc/regexp/regexp4.html
[46] https://github.blog/engineering/architecture-optimization/the-technology-behind-githubs-new-code-search/
[61] https://4.4.sourcegraph.com/code_navigation/explanations/features
[70] https://github.com/sourcegraph/sourcegraph-public-snapshot/blob/main/doc/dev/background-information/architecture/life-of-a-search-query.md
[71] https://webflow.sourcegraph.com/blog/sourcegraph-3-7
[75] https://sourcegraph.com/blog/keeping-it-boring-and-relevant-with-bm25f
[91] https://www.sqlite.org/fts5.html
[93] https://www.postgresql.org/docs/current/gin.html
[103] https://www.postgresql.org/docs/current/textsearch-tables.html
[107] https://cwiki.apache.org/confluence/display/lucene/NearRealtimeSearch
[109] https://www.elastic.co/docs/manage-data/data-store/near-real-time-search
[117] https://lucene.apache.org/core/9_7_0/core/org/apache/lucene/index/package-summary.html
[121] https://github.com/quickwit-oss/tantivy/blob/main/ARCHITECTURE.md
[126] https://docs.rs/tantivy/latest/tantivy/
[133] https://quickwit.io/blog/tantivy-0.22
[136] https://meilisearch.com/docs/resources/internals/storage
[139] https://www.meilisearch.com/docs/resources/self_hosting/configuration/reference
[142] https://www.meilisearch.com/docs/resources/help/faq
[147] https://typesense.org/docs/guide/system-requirements.html
[151] https://quickwit.io/blog/quickwit-101
[152] https://quickwit.io/docs/main-branch/overview/architecture
[155] https://quickwit.io/docs/overview/concepts/indexing
[158] https://quickwit.io/blog/benchmarking-quickwit-engine-on-an-adversarial-dataset
[163] https://quickwit.io/blog/quickwit-lambda-beta
[166] https://github.com/blevesearch/bleve/blob/master/index/scorch/README.md
[168] https://github.com/blevesearch/bleve/blob/master/docs/vectors.md
[169] https://vespa.ai/ai-search-platform/architecture/realtime-indexing/
[174] https://docs.vespa.ai/en/content/proton.html
[179] https://docs.vespa.ai/en/writing/partial-updates.html
[181] https://typesense.org/docs/guide/system-requirements.html
[185] https://github.com/typesense/typesense/blob/v28/README.md
[197] https://meilisearch.com/docs/capabilities/indexing/overview
[211] https://xapian.org/docs/apidoc/html/classXapian_1_1WritableDatabase.html
[213] https://xapian.org/docs/sourcedoc/html/glass__database_8h.html
[215] https://xapian.org/docs/bindings/perl/Xapian/WritableDatabase.html
[226] https://sourcegraph.com/blog/announcing-scip
[227] https://github.com/sourcegraph/scip
[238] https://sourcegraph.com/blog/announcing-scip-clang
[241] https://git-scm.com/docs/git-grep
[243] https://www.kernel.org/pub/software/scm/git/docs/git-log.html
[256] https://tree-sitter.github.io/tree-sitter/4-code-navigation.html
[260] https://sourcegraph.com/blog/keeping-it-boring-and-relevant-with-bm25f
[262] https://github.com/github/code-navigation/blob/main/README.md
[272] https://github.blog/engineering/architecture-optimization/the-technology-behind-githubs-new-code-search/
[292] https://typesense.org/docs/30.2/api/vector-search.html
[311] https://github.com/github/codesearchnet
[316] https://github.com/hound-search/hound?search=1
[318] https://deepwiki.com/hound-search/hound/3.2-indexing-engine
[331] https://github.com/livegrep/livegrep/blob/main/README.md
[333] https://blog.nelhage.com/2015/02/regular-expression-search-with-suffix-arrays/
[341] https://livegrep.com/about
[346] https://burntsushi.net/ripgrep/
[353] https://github.com/burntsushi/ripgrep
[361] https://github.com/sourcegraph/zoekt/blob/main/doc/design.md
[366] https://sourcegraph.com/docs/code-search/features
[376] https://github.com/sourcegraph/zoekt/blob/main/doc/design.md
[381] https://sourcegraph.com/docs/admin/deploy/kubernetes/scale
[391] https://github.com/sourcegraph/zoekt/issues/791
[392] https://gitlab.sgdev.org/root/sourcegraph/-/issues/29731
