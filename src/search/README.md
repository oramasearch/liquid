# Local search index implementation

The private search implementation stores content-addressed blobs, immutable full or delta
segments, and one atomically replaced canonical-CBOR manifest under `.liquid/index/`. A query reads
the manifest once and then only immutable files, so publication exposes either the prior path set or
the complete next path set.

`index.cpp` ports lake's positional trigram postings and uses the pinned oramacore-cpp tokenizer
and BM25F scorer for content and path ranking. Persisted term-frequency and trigram files are
written once for each blob even when several paths reference it. The built-in daemon target appends
one delta segment per delivered drip; the generic fan-out publishes its cursor only after that
segment's manifest is published.

`repository_access.hpp` exposes only the private index directory to the implementation of the
public `repository::search` and `repository::plan_search` operations in `repository.cpp`. The
planner classifies the query, measures active bytes, blobs, capture-cursor freshness, and trigram
candidate selectivity, then combines them with the machine's persisted scan calibration. Ranked
text has only a BM25F result set. Literal and regex searches choose between verified trigrams and a
current scan; stale working indexes cannot serve either query kind.

The scan executor enumerates checkout sources without publishing them, maps each source read-only,
and runs chunks as runtime actions. Literal cores reject regex misses before the standard regex
engine. All three executors render the same blob, path, line, column, snippet, and optional-score
record. Keeping the adapter separate lets
shared-library tests compile the private index core without exporting its helpers. The CLI and
library consumers never depend on the on-disk codec or oramacore types.
