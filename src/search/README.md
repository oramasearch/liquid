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
public `repository::search` operation in `repository.cpp`. Keeping that adapter separate lets
shared-library tests compile the private index core without exporting its helpers. The CLI and
library consumers never depend on the on-disk codec or oramacore types.
