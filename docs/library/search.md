# Local search index

**Status: implemented by liquid #22.** Each repository has a content-addressed BM25F and
positional-trigram index, updated by the daemon after each drip. The public repository API and
`liquid search` query the working tree or a named reference.

## Location and publication

The index is stored under `.liquid/index/`. The earlier `LIQUID_REPO/index/` design cannot be used:
`LIQUID_REPO` is the repository checkpoint file, not a directory. Search data is derived and can be
rebuilt from repository objects and the working tree.

Publication uses immutable change segments and one canonical-CBOR `manifest.cbor`. A segment maps
paths to blob digests or tombstones. A full publication replaces a slot's segment list; a drip
appends a delta segment. Blob, term, trigram, catalog, and segment files are complete before an
atomic rename replaces the manifest. A query reads the manifest once, so it observes either the
complete previous generation or the complete next generation.

The manifest has a `working` slot and separate `ref:<name>` slots. Reference indexes are populated
on first query from the immutable snapshot. Working-tree search is populated on first query when a
daemon index is not present.

## Content-addressed files

The layout is:

```text
.liquid/index/
  manifest.cbor
  segments/segment-<digest>.cbor
  blobs/<algorithm>-<digest>.blob
  terms/<algorithm>-<digest>.term
  trigrams/<algorithm>-<digest>.tri
  catalog/<segment>-<shard>.cat
```

Blob, term-frequency, and positional-trigram files are keyed only by the verified BLAKE3 content
digest. Two paths with identical bytes therefore share all three files. Segment occurrence records
retain the path-to-blob relationship without copying content.

Ranked queries use the tokenizer and BM25F scorer from `oramasearch/oramacore-cpp`, pinned at
`9b74ce177f47d63024532ae6000c2cd62c73e0cd`. Path and content field contributions are combined
before BM25F saturation, with a path weight of 3 and content weight of 1. Term statistics are
persisted once per blob instead of re-tokenizing the corpus for each process.

Quoted queries use the positional trigram representation ported from lake #6. Each blob file maps
a trigram to its byte positions. Immutable sharded catalog files prefilter candidate blobs; the
position lists then prove that all query trigrams occur contiguously. A slash-delimited regex made
only of literal characters uses that literal as the trigram prefilter. Regexes containing operators
scan active blobs before verification with the standard regex engine, avoiding false negatives from
alternation or optional terms. Queries shorter than three bytes also fall back to the active blobs.

## Daemon updates

`search` is a built-in daemon fan-out target. Its first delivery scans the checkout and publishes a
full working slot. Later deliveries read only the paths named by the drip, writing new documents or
tombstones. `.git`, `.liquid`, repository marker files, and local build directories are excluded.

The generic fan-out persists the search target cursor only after publication succeeds. Duplicate
cursor delivery is ignored, and a failed publication leaves the prior cursor for retry. Search is
therefore updated once for each accepted drip even when capture replays a delivery.

## API and CLI

```cpp
struct search_options {
    std::optional<std::string> ref;
    std::uint64_t limit{20};
};

struct search_hit {
    std::string path;
    digest blob;
    std::string snippet;
    double score;
};

runtime::task<std::vector<search_hit>> repository::search(
    std::string query,
    search_options options = {});
```

`liquid search <query> [--ref <name>] [--limit <count>]` prints score, path, and a one-line
snippet. Plain text selects BM25F. Quote a phrase for exact substring search or wrap a standard
regular expression in slashes for regex search.

## Verification and measurement

`SearchIndex` tests cover public working and named-reference queries, one stored blob shared by two
paths, contiguous positional trigrams, atomic manifest replacement under concurrent queries, a
live daemon drip becoming searchable, and one cursor advance after duplicate delivery.

`liquid-search-measure` indexed liquid's own checkout, excluding generated and repository-control
directories, then ran five representative BM25F, substring, and regex queries twice:

```text
search index build: files=3763 unique_blobs=3623 bytes=46266382 elapsed_ms=22214
search query mean: iterations=10 mean_us=268239
```

## Deferred

Searching inside packaged archives remains with the Phase 5 format registry. Issue #22 cannot
identify decomposable artifact bodies until that registry defines the supported formats and their
canonical extraction behavior. Symbol-aware planning and cost explanations remain #66 and #67.
