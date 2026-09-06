# Local search index

**Status: implemented by liquid #22 and #67.** Each repository has a content-addressed BM25F and
positional-trigram index, updated by the daemon after each drip. A measured cost planner selects
BM25F, verified trigrams, or a current working-set scan. The public repository API and
`liquid search` expose both results and the executed plan.

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

Quoted queries use a positional trigram representation. Each blob file maps
a trigram to its byte positions. Immutable sharded catalog files prefilter candidate blobs; final
literal or regex evaluation against the blob bytes verifies every result. A slash-delimited regex made
of literal characters uses that literal as the trigram prefilter. A conservative parser extracts a
required literal core from regexes without alternation or optional groups. Other regexes verify all
active blobs, avoiding false negatives. Queries shorter than three bytes also verify all active
blobs. Catalog readers open only shards named by the active manifest and binary-search their fixed
records.

## Executors and cost model

All executors return `search_hit`: blob identity, path, one-based line and column, a bounded
snippet, and an optional score. BM25F is the only ranked executor, so its score is populated and no
scan alternative is eligible for ranked text. Literal and regex results are unranked and identical
between the verified trigram executor and scan.

The scan executor enumerates the current checkout without publishing an index generation. Runtime
actions divide the blobs into hardware-sized chunks. Each action maps files or indexed blob objects
read-only, applies a literal prefilter before regex verification when possible, and returns ordinary
hits for a deterministic merge. It creates no worker threads and does not call `block_on`.

The planner measures or derives every input it reports:

- working-set path, unique-blob, and byte counts come from the active manifest, or from the current
  checkout when the manifest is stale or absent;
- freshness is the count of length-framed capture journal records after the cursor stored in the
  active index generation;
- query classification distinguishes ranked text, literal text, regex with a required literal
  core, and regex without one;
- trigram selectivity is active candidate blobs divided by unique blobs;
- scan cost is calibrated on first use and stored as
  `.liquid/index/scan-calibration`; the file records nanoseconds per sampled mapped blob on that
  machine, with a synthetic 64 KiB blob only when no working sample is available;
- scan parallelism is the smaller of scan work units and the runtime's hardware concurrency.

For `B` unique blobs, `Y` bytes, calibrated cost `N` nanoseconds, and parallelism `P`, scan work is
`B + ceil(Y / 65536)` and estimated microseconds are
`ceil(N * work / P / 1000)`. The index curve below measured a 350 microsecond fixed cost and a
650,000 nanosecond verified-candidate cost; the trigram estimate is therefore
`350 + ceil(650000 * selectivity * B / 1000)`. BM25F uses the same measured fixed cost plus 65
microseconds per blob. Ineligible choices remain in the plan with the reason they cannot preserve
the requested result set.

An index cursor behind the capture cursor is never queried for literal or regex results: scan wins
regardless of its estimated cost. Ranked text has no equivalent scan result, so the working index is
rebuilt to the current capture cursor before BM25F executes. The plan retains the observed drip
count and records that refresh as its reason.

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
    std::uint64_t line;
    std::uint64_t column;
    std::string snippet;
    std::optional<double> score;
};

struct search_plan {
    search_executor executor;
    search_query_kind query_kind;
    std::uint64_t estimated_cost_us;
    std::uint64_t actual_cost_us;
    // measured inputs and all eligible/ineligible alternatives
};

runtime::task<std::vector<search_hit>> repository::search(
    std::string query,
    search_options options = {});

runtime::task<search_plan> repository::plan_search(
    std::string query,
    search_options options = {});
```

`liquid search <query> [--ref <name>] [--limit <count>]` prints optional score, path, line,
column, and a one-line snippet. Plain text selects BM25F. Quote a phrase for exact substring search
or wrap a standard regular expression in slashes for regex search. `--explain` executes the query
and prints stable JSON containing the chosen executor, query kind, estimated and actual
microseconds, measured inputs, and every alternative. The API's `plan_search` returns the same
executed plan.

## Verification and measurement

`SearchIndex` and `SearchPlanner` tests additionally compare the complete literal and regex result
identity across scan and trigram execution, pin stable explain JSON, assert scan and index choices on
opposite sides of the measured threshold, and prove that a drip after publication disables a stale
index answer.

`liquid-search-measure` indexed liquid's own checkout, excluding generated and repository-control
directories, then ran five representative BM25F, substring, and regex queries twice:

```text
search index build: files=3812 unique_blobs=3672 bytes=46572788 elapsed_ms=22629
search query mean: iterations=10 mean_us=488424
```

The final #67 run on the same Apple M3 Max host used load average `{ 168.25 115.22 82.68 }`.
Active-only catalog lookup and verified candidate reads reduced the ten-query mean to 207,101
microseconds:

```text
search index build: files=3820 unique_blobs=3680 bytes=46850094 elapsed_ms=21910
search query mean: iterations=10 mean_us=207101
```

The same executable forced one mapped scan action and the verified trigram path for the literal
`"query graph"` and regex `/query graph.*/`. The table records one observation at each working-set
size; the planner applies measured runtime parallelism to the scan column.

| Files | Selectivity | Literal scan µs | Literal index µs | Regex scan µs | Regex index µs |
|---:|---:|---:|---:|---:|---:|
| 10 | 0.200000 | 181 | 300 | 1,052 | 1,137 |
| 50 | 0.260000 | 805 | 790 | 2,158 | 2,222 |
| 100 | 0.290000 | 1,558 | 2,748 | 5,703 | 5,729 |
| 250 | 0.196000 | 4,094 | 6,908 | 14,320 | 15,153 |
| 500 | 0.106157 | 9,348 | 6,436 | 16,630 | 16,210 |
| 1,000 | 0.058824 | 17,919 | 14,001 | 25,821 | 24,693 |
| 2,000 | 0.030287 | 36,431 | 17,673 | 42,163 | 29,747 |
| 3,820 | 0.016848 | 68,116 | 25,253 | 74,984 | 34,694 |

The sustained literal crossover is between 250 and 500 files; the 50-file index observation is an
isolated 15-microsecond inversion. Regex scan is cheaper through 250 files and the index remains
cheaper from 500 onward. The planner tests use 10 and 4,000 blobs to stay well outside those
transition bands and assert both executor choices.

## Deferred

Searching inside packaged archives remains with the Phase 5 format registry. Issue #22 cannot
identify decomposable artifact bodies until that registry defines the supported formats and their
canonical extraction behavior. Symbol-record lookup is documented separately in
[`symbols.md`](symbols.md).
