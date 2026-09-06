# libliquid API map

**What.** The public surface of `liquid::liquid` for Phase 1 (the store) and the rules every
header under `include/liquid/` obeys. This is the design liquid issue #39 asked for; the headers
are its implementation, and a header that is not on this map is not public.

**Why.** liquify and lake link libliquid today and will call the store, the object model and the
drip stream as soon as they exist. The shape has to be decided before the store is written: an
error model, a reference type that carries its hash algorithm, opaque handles, and a
concurrency rule are all cheap now and expensive to retrofit. The map also says what is
*deliberately internal*, which is what keeps Zetta, Boost and the engine choices out of the API.

## Rules for every public header

1. **Errors are values.** Every function that can fail returns `liquid::result<T>`, which is
   `std::expected<T, liquid::error>`. No exception crosses the boundary; anything thrown inside is
   caught at the boundary and becomes `errc::internal` with the message preserved.
2. **Handles are opaque.** Stateful things (`store`, `repository`, `capture`, `lease`) are classes
   whose only data member is a pointer to a private implementation. Their layout never changes,
   so a shared libliquid can change its internals without recompiling clients.
3. **Values are plain.** Records (`digest`, `tree_entry`, `change`, `drip`, `error`) are aggregates
   of standard types: `std::string`, `std::vector`, `std::array`, `std::optional`, `std::span`,
   `std::chrono`. They may gain fields; they never lose or reorder them before 1.0 is declared.
4. **No third-party type is visible, with one exception.** Not Zetta, ever. Not Corosio,
   automerge-cpp, zstd, BLAKE3, tree-sitter: they live behind the handles. The exception is
   **Boost.Capy, the coroutine substrate** (Steve, 2026-09-03: "use the coroutine library that we
   have"): its `task` and `io_result` reach clients through `liquid/runtime.hpp`, the one header
   that names Capy, so a substrate change is a change there and nowhere else (Zetta's pattern).
5. **Every reference carries its algorithm.** A `digest` is `(algorithm, bytes)`; its text form is
   `blake3:<hex>`, `sha256:<hex>`, or private-store `blake3k:<hex>`. Nothing in the API takes bare
   bytes as an identity.
6. **Coroutine-native.** Every function that performs I/O or may wait returns
   `liquid::runtime::task<T>` (`boost::capy::task<liquid::result<T>>`) and is `co_await`ed; pure computation
   (hashing, chunking, encoding, parsing) is synchronous and returns `result<T>` directly. liquid
   tasks run on a Corosio `io_context` executor: a client that owns one (lake, liquify) awaits
   liquid tasks from its own coroutines; a client that does not (the CLI, tests) uses
   `runtime::block_on`, which owns one. Blocking third-party work goes through `runtime::offload`,
   never a raw thread. Errors stay in `result<T>`; composition with Capy's `when_all`/`timeout`
   goes through the adapters in `runtime.hpp`.
7. **Exported explicitly.** Every public class and free function carries `LIQUID_EXPORT`. Templates
   and inline functions in public headers touch only public types; nothing inline reaches private
   state.
8. **One umbrella.** `liquid/liquid.hpp` includes every public header; clients may include either.

## The headers

`implemented` means the operation performs its documented work. `declared` means the public type
and signatures compile and link, with callable operations returning `errc::unsupported` and the
owning issue in `error::context`.

| Header | Phase | Status | What it exposes |
|---|---|---|---|
| `liquid/liquid.hpp` | now | implemented | umbrella |
| `liquid/version.hpp` | now | implemented | `version`, `current_version()`, `version_string()`, `grammar_digest()` |
| `liquid/export.hpp` | now | implemented | generated; `LIQUID_EXPORT` |
| `liquid/runtime.hpp` | #97, #193 | implemented | the Capy boundary: `task<T>`, `io_result`, `when_all`, `timeout`, `delay`, `as_io`, `block_on`, `spawn`, `spawn_serial`, `offload`, `context` |
| `liquid/error.hpp` | #39, #200 | implemented | `errc`, including the named unconfigured-build result, `error`, `result<T>` |
| `liquid/flow.hpp` | #85 | implemented | persisted actor/document/repository bucket configuration, watermark configuration, pause reasons, and live daemon bucket status |
| `liquid/attestation.hpp` | #14, #61, #62 | implemented | registered TAAP profile schemas, typed attributes and commitments, structured SLSA v1 provenance, signed SBOM claims, attestation encoding, and profile-keyed verification |
| `liquid/abi.hpp` | #27 | implemented | synchronous ELF/Mach-O/PE ABI extraction, stable descriptors, compatibility verdicts and change reports, and JSON projection |
| `liquid/hash.hpp` | #39, #7 | implemented | `algorithm`, `digest`, `hash(span) -> digest`, keyed-id text round trip |
| `liquid/asn1.hpp` | #210 | implemented | strict DER element reader with exact encoded spans plus unsigned-integer and object-identifier decoding |
| `liquid/x509.hpp` | #210 | implemented | certificate parsing, explicit TSA trust anchors, historical path validation, key usage, and critical timestamp-signing purpose |
| `liquid/cms.hpp` | #210 | implemented | embedded CMS SignedData content, signed-attribute, signer, signature, and historical certificate-path verification |
| `liquid/transparency.hpp` | #207, #210 | implemented (#210) | RFC 3161 timestamp-token and RFC 4998 evidence-chain verification over checkpoint signature values with explicit TSA trust anchors |
| `liquid/graph.hpp` | #20, #21, #23 | implemented | registered coroutine query kinds, canonical keys, recorded input access, persisted red-green demand, durability invalidation, cancellation, bounded resident values, statistics, clean validation, audit records, and stale-value eviction |
| `liquid/frontend_abi.h` | #65, #66 | implemented | versioned C module descriptor, language registration, owned bytes, progress/cancellation callbacks, and the `units`, `compile_unit`, `assemble`, and `symbols` operations |
| `liquid/frontend.hpp` | #65, #66, #193 | implemented | checked shared/static frontend loading, language registry, source units, symbol-record codec, the unit-keyed query-graph coordinator, and the shared diagnostic value used by build events |
| `liquid/git.hpp` | #43 | implemented | Git repository import and export reports and operations |
| `liquid/store.hpp` | #1, #7, #8, #17, #46, #53, #58, #64, #116, #199 | implemented | `store`: memory and streaming-file content in and out; pre-pack write-through; local/remote/absent blob availability, local eviction, and lazy remote ranges; encrypted trust domains, key-version, and subject-attestation lookup; leases, collection, rule counts, and pack compaction |
| `liquid/node_store.hpp` | #202 | implemented | per-node shared chunk store, metadata-only image receipt, reference-counted read-only image mounts, hydration/reuse counters, logical byte budget and remote-content eviction, local snapshotter API, and TCP control surfaces |
| `liquid/delta.hpp` | #64 | implemented | content-addressed delta records, zstd reference patches, verified create/apply, and fetch through local or registered remote blobs with chunk fallback |
| `liquid/object.hpp` | #2, #17, #50, #62 | implemented | node kinds and canonical record values: local and remote `blob`, `tree`, `change`, attestation references, repository references, conflicts, and reserved symbols |
| `liquid/repository.hpp` | #2, #11, #16, #17, #22, #27, #40, #41, #42, #50, #52, #53, #58, #66, #67, #116, #160, #165, #166, #174, #193, #195, #200 | implemented | `repository`: open on local or caller-owned stores; current tree digest; encrypted, plaintext, and remote reads; memory, streaming-file, and remote-record writes; content-addressed symbol and ABI records; cost-planned working/ref search with common located hits and executed-plan explanations; redaction, structural key diff, public tree/change diff and text-anchor mapping, path history and restore, recorded moves/removals, cuts, refs, status including remote bytes, history, readability, nest/split/combine, conflicts, undo, locks, paged result/attestation/trace/SBOM enumeration, ABI lookup, cache statistics, configured build plans and one-call builds, direct build handles, test target registration/enumeration/execution, persisted REAPI remote configuration, typed retention policy, retention explanations, and bounded signed sync export/import |
| `liquid/resolve.hpp` | #25, #27 | implemented | plain spec/package/concretisation values, canonical record codecs, filesystem and repository universe loading, reuse, known-output and ABI-compatibility inputs, asynchronous CDCL resolution, minimal attributed conflict cores, stable explanations and JSON |
| `liquid/repository.hpp` | #2, #11, #16, #17, #22, #27, #40, #41, #42, #50, #52, #53, #58, #66, #67, #116, #160, #165, #166, #174, #193, #200 | implemented | `repository`: open on local or caller-owned stores; current tree digest; encrypted, plaintext, and remote reads; memory, streaming-file, and remote-record writes; content-addressed symbol and ABI records; cost-planned working/ref search with common located hits and executed-plan explanations; redaction, structural key diff, public tree/change diff and text-anchor mapping, recorded moves/removals, cuts, refs, status including remote bytes, history, readability, nest/split/combine, conflicts, undo, locks, paged result/attestation/trace/SBOM enumeration, ABI lookup, cache statistics, configured build plans and one-call builds, direct build handles, test target registration/enumeration/execution, persisted REAPI remote configuration, typed retention policy, retention explanations, and bounded signed sync export/import |
| `liquid/repository.hpp` | #2, #11, #16, #17, #22, #27, #40, #41, #42, #50, #52, #53, #58, #66, #67, #85, #86, #116, #160, #165, #166, #174, #193, #200 | implemented | `repository`: open on local or caller-owned stores; current tree digest; encrypted, plaintext, and remote reads; flow-controlled memory, streaming-file, and remote-record writes; content-addressed symbol and ABI records; cost-planned working/ref search with common located hits and executed-plan explanations; stale-base validation; redaction, structural key diff, public tree/change diff and text-anchor mapping, recorded moves/removals, cuts, refs, status including remote bytes, history, readability, nest/split/combine, conflicts, undo, locks, paged result/attestation/trace/SBOM enumeration, ABI lookup, cache statistics, configured build plans and one-call builds, direct build handles, test target registration/enumeration/execution, persisted REAPI remote, agent, and flow configuration, typed retention policy, retention explanations, and bounded signed sync export/import |
| `liquid/drip.hpp` | #4, #5, #48, #50 | implemented (#5, #50); declared (#4, #48) | `drip`, `capture`: the scanned journal as a subscription, safe block events, pause and resume |
| `liquid/identity.hpp` | #4, #7, #51, #53, #111 | implemented (#4, #7, #53, #111); declared (#51) | TAAP `actor`, `signer`, identity rotation and chain verification, versioned data-key rotation, keyring and custody providers |
| `liquid/vfs.hpp` | #6 | implemented | `vfs::mount`: FUSE on Linux and optional macFUSE on macOS; `vfs::materialise`: full checkout on every platform |
| `liquid/build.hpp` | #10, #11, #12, #13, #14, #15, #16, #17, #18, #19, #23, #26, #27, #57, #60, #56, #58, #61, #193, #200 | implemented local and REAPI execution, wrapper, disk and remote cache, verification, replay, provenance, diagnostics, observability, event streams, content-addressed outputs, public plans, input policy, configure facts, retention, tests, SBOMs, and ABI publication | action execution and results, named upstream result/tree inputs, canonical output-tree claims, signature-valid `known_outputs` enumeration, complete configured plan entries that preserve named input trees, stable JSON event projection, move-only build handles and subscriptions, test target/result values, remote eligibility and read refusal, recorded cost decisions, batching and racing, typed cache and retention explanations, critical alerts, retained build traces, critical paths, Chrome export, policy checking, configure fact inspection/export, foreign-target wrapping, action and graph verification, explicit replay through registered remote sources, provenance verification, JSON/DSSE projection, tier and byte reports, graph-derived SPDX/CycloneDX SBOMs, marked binary inference, and wrapper ABI-break reports; no sandbox or index implementation types are public |
| `liquid/daemon.hpp` | #21, #23, #193, #200 | implemented on macOS and Linux; documented unsupported result on Windows | daemon lifecycle, ordered pluggable targets, per-target cursor status, capture controls, graph-memory and validation status, idle clean validation, configured plan retrieval, repository and direct-action build stream forwarding, attach, and shutdown |
| `liquid/build.hpp` | #10, #11, #12, #13, #14, #15, #16, #17, #18, #19, #23, #27, #57, #60, #56, #58, #61, #193, #200 | implemented local and REAPI execution, wrapper, disk and remote cache, verification, replay, provenance, diagnostics, observability, event streams, public plans, input policy, configure facts, retention, tests, SBOMs, and ABI publication | action execution and results, complete configured plan entries and stable JSON projection, move-only build handles and subscriptions, stable JSON event projection, test target/result values, remote eligibility and read refusal, recorded cost decisions, batching and racing, typed cache and retention explanations, critical alerts, retained build traces, critical paths, Chrome export, policy checking, configure fact inspection/export, foreign-target wrapping, action and graph verification, explicit replay through registered remote sources, provenance verification, JSON/DSSE projection, tier and byte reports, graph-derived SPDX/CycloneDX SBOMs, marked binary inference, and wrapper ABI-break reports; no sandbox or index implementation types are public |
| `liquid/daemon.hpp` | #21, #23, #193, #195, #200 | implemented on macOS and Linux; documented unsupported result on Windows | daemon lifecycle, ordered pluggable targets including configured drive sync, per-target cursor status, capture controls, graph-memory and validation status, idle clean validation, configured plan retrieval, repository and direct-action build stream forwarding, attach, and shutdown |
| `liquid/daemon.hpp` | #21, #23, #85, #193, #200 | implemented on macOS and Linux; documented unsupported result on Windows | daemon lifecycle, ordered pluggable targets, per-target cursor status, flow watermarks and in-process consumer lag, capture controls, graph-memory and validation status, idle clean validation, configured plan retrieval, repository and direct-action build stream forwarding, attach, and shutdown |
| `liquid/remote.hpp` | #16, #17, #18, #63 | implemented | REAPI endpoint and liquid-scheduler backend selection, TLS, header, deadline, batching, and publication configuration; `remote_blob_location`; transport-neutral source and executor operations including prepared scheduling records; and the move-only negotiated client handle |
| `liquid/reapi.hpp` | #16 | implemented | `reapi_serve_options` and the coroutine-native repository CAS/ActionCache server entry point |
| `liquid/workers.hpp` | #63 | implemented | scheduler and worker configuration plus coroutine-native long-running scheduler and worker entry points |
| `liquid/sync.hpp` | #160, #165, #177, #189, Phase 6 | implemented | signed metadata items and reports, change-graph-derived per-ref authorization and outcomes, atomic push selection, public repository `sync_server`, missing-object `sync_session`, and the HTTP/1.1 `sync_endpoint` used by CLI push/pull and lake clients |
| `liquid/secret.hpp` | #204 | implemented | X25519 sealing keys, exact deployment scopes, canonical-CBOR sealed-secret encode/decode, synchronous seal/unseal, keypair generation, and coroutine-native Lake public-key fetch |
| `liquid/sync.hpp` | #160, #165, #177, #189, #202, Phase 6 | implemented | signed metadata items and reports, change-graph-derived per-ref authorization and outcomes, atomic push selection, public repository `sync_server`, whole-graph and metadata-only `sync_session`, and the HTTP/1.1 `sync_endpoint` used by CLI push/pull, node stores, and lake clients |
| `liquid/sync.hpp` | #85, #160, #165, #177, #189, Phase 6 | implemented | signed metadata items and reports, peer flow rejection and retry reports, change-graph-derived per-ref authorization and outcomes, atomic push selection, public repository `sync_server`, missing-object `sync_session`, and the HTTP/1.1 `sync_endpoint` used by CLI push/pull and lake clients |

### `liquid/error.hpp`

```cpp
enum class errc : std::uint16_t {
  ok = 0, not_found, already_exists, conflict, invalid_argument, unsupported,
  corrupt, io, locked, unauthorized, unavailable, busy, internal, not_remote
};
struct error { errc code; std::string message; std::string context; };  // context: a path, a digest, an op id
template <class T> using result = std::expected<T, error>;
std::string_view to_string(errc);
```

### `liquid/flow.hpp`

```cpp
struct token_bucket_config { std::uint64_t rate_per_s, burst; };
struct flow_config {
  token_bucket_config per_actor, per_document, per_repository;
  std::map<std::string, token_bucket_config> actor_overrides;
  std::uint64_t max_update_bytes, max_unacknowledged, pause_above, resume_below;
};
enum class flow_scope { actor, document, repository };
struct flow_bucket_status {
  flow_scope scope; std::string key; double tokens;
  std::uint64_t rate, burst, rejected;
};
enum class flow_pause_reason { none, persistence, consumers };
struct flow_status {
  bool paused; flow_pause_reason reason;
  std::uint64_t persistence_lag, consumer_lag;
  std::vector<flow_bucket_status> buckets;
};
```

`flow_config` is persisted under `config.flow`. Live token balances and rejection counts are
runtime status, not repository history.

`unsupported` is the answer of every function whose phase has not landed: the header compiles and
links from day one, and callers can be written against it before the store exists.

### `liquid/runtime.hpp`

```cpp
namespace liquid::runtime {
template <class T> using task = boost::capy::task<result<T>>;   // every I/O function returns this
using boost::capy::io_result;  using boost::capy::when_all;  using boost::capy::timeout;  using boost::capy::delay;
template <class T> boost::capy::io_task<result<T>> as_io(task<T>); // adapt a liquid task for Capy composition
class context;                        // owns a Corosio io_context and its threads; opaque
boost::capy::any_executor context::executor() const;
context& default_context();           // process-wide shared context for spawned I/O work
template <class T> result<T> block_on(task<T>);                // run to completion on a private context (CLI, tests)
void spawn(task<void>, std::function<void(result<void>)> on_done);   // fire-and-forget on the shared runtime context
void spawn_serial(task<void>, std::function<void(result<void>)> on_done); // fire-and-forget on a single-runner I/O context
template <class F> task<std::invoke_result_t<F>> offload(F blocking); // blocking CPU work on the compute pool; resumes on the caller's executor
detail::yield_awaiter yield() noexcept;                            // reschedule through the caller's executor; no timer, no stop callback
}
```

Corosio never appears here; `src/runtime/net.hpp` is the private adapter TUs include for
sockets, timers and files. Coroutine functions take their parameters **by value** and are never
capturing lambdas (the frame outlives the call). A task is lazy and `[[nodiscard]]`; after
`offload` the continuation is back on the caller's executor before any Corosio object is touched.
`block_on` is for synchronous entry points only and must not be called from a coroutine.

| Entry point | Intended use | Executor / ownership |
|---|---|---|
| `block_on` | CLI and tests | a new private context, run to task completion |
| `spawn` | long-lived I/O work | the process-wide shared runtime context |
| `spawn_serial` | ordered long-lived I/O work | the process-wide single-runner event context |
| `offload` | blocking CPU work | the compute pool; resumes on the caller executor |
| `yield` | let other work run between steps of a long loop | posts the continuation to the caller executor |

### `liquid/hash.hpp`

```cpp
enum class algorithm : std::uint8_t { blake3 = 1, sha256 = 2, blake3_keyed = 3 };
struct digest { algorithm alg; std::array<std::byte, 32> bytes; };   // fixed width; both are 256-bit
result<digest> hash(algorithm, std::span<const std::byte>);
std::string to_string(const digest&);                 // "blake3:9f86d0..." / "blake3k:..."
result<digest> parse_digest(std::string_view);
```

BLAKE3 is the default everywhere; SHA-256 exists for OCI and REAPI interop. `blake3_keyed` is
reserved for store-generated trust-domain MAC identifiers: callers can parse and display it, while
`hash(blake3_keyed, ...)` rejects input because the key remains inside the store. Adding an
algorithm is adding an enumerator; a repository may hold multiple algorithms (liquid #44).

The BLAKE3 implementation is the official C implementation, fetched privately at release tag
`1.8.7` and compiled in portable mode. Its headers do not enter `include/liquid/`. SHA-256 is the
small public-domain implementation by Brad Conte from
[`B-Con/crypto-algorithms`](https://github.com/B-Con/crypto-algorithms), vendored from commit
`cfbde48414baacf51fc7c74f275190881f037d32` under `src/hash/`. Upstream states that the code is
released into the public domain without restrictions. The local copy adds C++ linkage guards,
fixed-width input promotion before shifts, and a `size_t` input loop; `NOTICE` records both
origins. The `abc` SHA-256 vector is from that implementation's `sha256_test.c`; BLAKE3 vectors
are checked against the official implementation.

### `liquid/graph.hpp`

```cpp
enum class durability : std::uint8_t { volatile_ = 0, low = 1, medium = 2, high = 3 };
enum class graph_input_kind : std::uint8_t {
  working_tree_blob = 1, overlay_buffer = 2, environment = 3, toolchain = 4
};
struct graph_input_key { graph_input_kind kind; std::string name; bool operator==(...) const; };
struct query_key { std::string kind; std::vector<std::byte> arguments; bool operator==(...) const; };
using graph_dependency_key = std::variant<query_key, graph_input_key>;
struct graph_value { std::vector<std::byte> bytes; digest fingerprint; durability durability_tier; };
struct recorded_dependency { graph_dependency_key key; digest fingerprint; };
struct recorded_query {
  query_key key; digest fingerprint; std::vector<recorded_dependency> dependencies; graph_value value;
};
struct graph_statistics {
  std::uint64_t demands, hits, executions, dependency_reads, fingerprint_reads;
  std::uint64_t invalidation_walks, discarded_computations;
  std::uint64_t keys, resident_values, resident_value_bytes, memory_ceiling;
};
struct graph_validation_record {
  digest id; query_key query; std::vector<recorded_dependency> inputs;
  digest incremental, clean; bool agreement; std::chrono::sys_seconds at;
};
struct graph_validation_options { std::uint32_t rate; bool all; std::uint64_t limit; };
struct graph_validation_status {
  std::uint64_t eligible, validated, backlog, records;
  std::optional<graph_validation_record> last_divergence;
};
struct graph_validation_report {
  std::uint64_t considered, validated, divergences, backlog; std::chrono::milliseconds elapsed;
  std::vector<graph_validation_record> records;
  std::optional<graph_validation_record> last_divergence;
};
using query_argument_encoder =
  std::function<result<std::vector<std::byte>>(std::span<const std::byte>)>;
using query_compute =
  std::function<runtime::task<std::vector<std::byte>>(query_context&, std::vector<std::byte>)>;
struct query_kind { std::string name; query_argument_encoder encode_arguments; query_compute compute; };

class query_registry {
 public:
  query_registry(); ~query_registry();
  query_registry(const query_registry&) noexcept;
  query_registry& operator=(const query_registry&) noexcept;
  query_registry(query_registry&&) noexcept;
  query_registry& operator=(query_registry&&) noexcept;
  result<void> add(query_kind);
  result<query_key> key(std::string, std::span<const std::byte>) const;
};
struct graph_options { std::filesystem::path working_tree; std::uint64_t memory_ceiling; };
class graph {
 public:
  graph() noexcept; ~graph();
  graph(graph&&) noexcept; graph& operator=(graph&&) noexcept;
  graph(const graph&) = delete; graph& operator=(const graph&) = delete;
  static runtime::task<graph> open(std::filesystem::path, query_registry, graph_options = {});
  runtime::task<graph_value> demand(query_key);
  runtime::task<void> set_input(graph_input_key, std::vector<std::byte>, durability);
  runtime::task<void> clear_input(graph_input_key, durability);
  result<std::uint64_t> mark_changed(graph_input_key, durability);
  result<std::uint64_t> mark_changed(query_key);
  runtime::task<std::optional<recorded_query>> recorded(query_key);
  runtime::task<graph_validation_report> validate(graph_validation_options = {});
  graph_validation_status validation_status() const;
  runtime::task<std::vector<graph_validation_record>> validation_records();
  runtime::task<void> inject_stale_value_for_testing(query_key, std::vector<std::byte>);
  runtime::task<void> flush();
  graph_statistics statistics() const;
};
class query_context {
 public:
  query_context(query_context&&) noexcept;
  query_context& operator=(query_context&&) noexcept;
  query_context(const query_context&) = delete;
  query_context& operator=(const query_context&) = delete;
  ~query_context();
  runtime::task<graph_value> read(graph_dependency_key);
};
```

`query_registry::key` runs the kind's canonical argument encoder; an absent encoder preserves the
provided bytes. A compute function is a Capy coroutine and performs every input or nested-query
read through `query_context::read`, which records dependencies in read order and checks cooperative
cancellation. `graph::open` shallow-loads persisted query metadata. `demand` validates stale nodes
red-green, cuts off unchanged fingerprints, and persists only complete computations. The two
`mark_changed` overloads return the exact invalidation walk count; the query overload also removes
its completed value to force recomputation. `set_input` and `clear_input` manage overlay,
environment, and toolchain leaves; working-tree leaves otherwise read relative to
`graph_options::working_tree`. A nonzero `graph_options::memory_ceiling` evicts resident values in
least-recently-demanded order while retaining keys, fingerprints, dependencies, and stored value
identifiers. `recorded` loads a completed value and returns the clean-comparison surface; a query
with no completed value returns an empty optional. `validate` samples completed nodes or runs a
full pass, computes each through a new empty graph, retains its comparison record, and evicts a
divergent value. `validation_status` is the non-I/O backlog and latest-divergence view;
`validation_records` returns the durable audit history. The explicitly named test hook corrupts a
completed value while preserving an otherwise valid graph node. `flush` atomically republishes the
current graph root, and `statistics` returns counters since open plus the current resident-value
accounting.

### `liquid/frontend_abi.h` and `liquid/frontend.hpp`

The module boundary is C11 and carries only fixed-width integers, byte spans, strings, function
pointers, and caller-independent owned bytes. Dynamic modules export
`liquid_frontend_get_module_v1`; static consumers pass the same descriptor to `linked`. Every
module operation receives optional progress and cancellation callbacks and returns canonical CBOR.

```c
#define LIQUID_FRONTEND_ABI_VERSION 1u
#define LIQUID_FRONTEND_ENTRYPOINT_V1 "liquid_frontend_get_module_v1"
typedef enum liquid_frontend_status_v1 {
  LIQUID_FRONTEND_STATUS_OK = 0,
  LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
  LIQUID_FRONTEND_STATUS_UNSUPPORTED,
  LIQUID_FRONTEND_STATUS_CANCELLED,
  LIQUID_FRONTEND_STATUS_INTERNAL
} liquid_frontend_status_v1;
typedef struct liquid_frontend_bytes_v1 { const uint8_t* data; size_t size; }
  liquid_frontend_bytes_v1;
typedef struct liquid_frontend_owned_bytes_v1 {
  uint8_t* data; size_t size; liquid_frontend_release_v1 release; void* context;
} liquid_frontend_owned_bytes_v1;
typedef struct liquid_frontend_callbacks_v1 {
  void* context; liquid_frontend_progress_v1 progress; liquid_frontend_cancelled_v1 cancelled;
} liquid_frontend_callbacks_v1;
typedef struct liquid_frontend_language_v1 {
  const char* name; const char* const* extensions; size_t extension_count;
} liquid_frontend_language_v1;
typedef struct liquid_frontend_module_v1 {
  uint32_t abi_version; size_t struct_size; const char* name;
  const liquid_frontend_language_v1* languages; size_t language_count; void* module_context;
  liquid_frontend_units_v1 units; liquid_frontend_compile_unit_v1 compile_unit;
  liquid_frontend_assemble_v1 assemble; liquid_frontend_destroy_v1 destroy;
} liquid_frontend_module_v1;
```

`units(language, blob, context)` returns an array of declaration, definition, and type maps with
names, source ranges, and source bytes. `compile_unit(unit, context)` returns a compiler action.
`assemble(unit_outputs, context)` returns the translation-unit output. The release callback owns
every successful result buffer; the loader copies and releases it before returning. The complete
canonical schemas are in [`frontends.md`](frontends.md).

```cpp
enum class frontend_diagnostic_severity { note, warning, error, fatal };
struct frontend_diagnostic { std::string file; std::uint64_t line, column;
                             frontend_diagnostic_severity severity; std::string message; };
enum class frontend_unit_kind : std::uint8_t { declaration = 1, definition = 2, type = 3 };
struct frontend_source_position { std::uint64_t byte, line, column; };
struct frontend_source_range { frontend_source_position start, end; };
struct frontend_unit {
  frontend_unit_kind kind; std::string name; frontend_source_range range;
  digest fingerprint; std::vector<std::byte> canonical_value;
};
struct frontend_callbacks {
  std::function<void(std::string_view, std::uint64_t, std::uint64_t)> progress;
  std::function<bool()> cancelled;
};
enum class frontend_tier_mechanism : std::uint8_t { frontend = 1, grammar = 2 };
struct frontend_language {
  std::string language; std::vector<std::string> extensions;
  frontend_tier_mechanism tier_mechanism; std::string grammar_digest;
};
class frontend_module {
 public:
  frontend_module() noexcept; ~frontend_module();
  frontend_module(frontend_module&&) noexcept;
  frontend_module& operator=(frontend_module&&) noexcept;
  static result<frontend_module> load(std::filesystem::path shared_object);
  static result<frontend_module> linked(const liquid_frontend_module_v1* descriptor);
  std::string_view name() const; std::vector<std::string> languages() const;
  std::vector<frontend_language> language_details() const;
  bool supports(std::string_view language) const;
  result<std::vector<frontend_unit>> units(std::string_view language,
      std::span<const std::byte> blob, std::span<const std::byte> context_cbor = {},
      frontend_callbacks = {}) const;
  result<std::vector<std::byte>> compile_unit(const frontend_unit&,
      std::span<const std::byte> context_cbor = {}, frontend_callbacks = {}) const;
  result<std::vector<std::byte>> assemble(std::span<const std::byte> unit_outputs_cbor,
      std::span<const std::byte> context_cbor = {}, frontend_callbacks = {}) const;
  result<std::vector<std::byte>> symbols(std::string_view language,
      std::span<const std::byte> blob, std::span<const std::byte> context_cbor = {},
      frontend_callbacks = {}) const;
  result<std::vector<std::byte>> symbols_from_output(std::string_view language,
      std::span<const std::byte> blob, std::span<const std::byte> action_output,
      frontend_callbacks = {}) const;
};
class frontend_registry {
 public:
  frontend_registry(); ~frontend_registry();
  frontend_registry(frontend_registry&&) noexcept;
  frontend_registry& operator=(frontend_registry&&) noexcept;
  result<void> add(frontend_module); result<void> load(std::filesystem::path shared_object);
  result<const frontend_module*> find(std::string_view language) const;
  std::vector<frontend_language> languages() const;
};
struct frontend_build_request {
  std::string language, source_path, compile_action; std::vector<std::byte> source, context_cbor;
};
struct frontend_unit_output {
  frontend_unit unit; std::vector<std::byte> action, output; bool action_ran;
};
struct frontend_build_report {
  std::vector<frontend_unit_output> units; std::vector<std::byte> translation_unit_output;
  std::uint64_t actions_run, assemblies_run;
};
class frontend_graph {
 public:
  frontend_graph() noexcept; ~frontend_graph();
  frontend_graph(frontend_graph&&) noexcept;
  frontend_graph& operator=(frontend_graph&&) noexcept;
  static runtime::task<frontend_graph> open(std::filesystem::path state_directory,
      std::filesystem::path working_tree, std::shared_ptr<frontend_module>,
      frontend_action_executor);
  runtime::task<frontend_build_report> build(frontend_build_request);
  runtime::task<symbol_record> symbols(frontend_build_request);
  result<std::uint64_t> mark_changed(std::string path, durability = durability::low);
  graph_statistics statistics() const;
};
```

`frontend_module` validates the descriptor, ownership contract, and canonical CBOR before values
cross into C++. Optional ABI-v1 language metadata identifies compiler frontends and grammar-backed
languages while preserving compatibility with the original descriptor size. `frontend_registry`
rejects duplicate language registrations and projects names, extensions, mechanism, and grammar
identity. `frontend_graph`
registers the action and assembly query kinds, records action read paths through `query_context`,
and runs the potentially blocking assembly operation through `runtime::offload`. The caller's
`frontend_action_executor` owns compiler-action scheduling and returns object bytes. All three
handles are move-only; their public copy operations are deleted.

`symbol_record` is `{ subject, language, origin, grammar, occurrences }`; `grammar` is empty for
compiler records and carries the canonical lock identity for syntactic records. Each occurrence carries its stable
identifier, display name, kind, definition/reference role, source range, and optional container and
documentation. `encode_symbols` and `decode_symbols` round-trip the canonical stored record.
`decode_frontend_symbols(subject, bytes)` binds a frontend operation's per-unit output to its blob
subject. `frontend_module::symbols(language, blob, context, callbacks)` describes a symbol action,
and `frontend_module::symbols_from_output(language, blob, stdout, callbacks)` decodes its action
output. `frontend_graph::symbols(request)` is separate from `build(request)` and invokes the
injected action executor only when the symbol query is demanded.
`repository::store_symbols(record)` stores and associates a record, while
`repository::symbols(subject)` returns the associated public value.

### `liquid/node_store.hpp`

```cpp
struct node_store_options {
  std::filesystem::path root;
  std::string listen{"unix:///run/liquid/node-store.sock"};
  std::string control_listen{"127.0.0.1:8082"};
  std::uint64_t budget_bytes{};
};
struct mount_request { digest tree; std::string store_url; std::string bearer; };
struct mount_result {
  std::filesystem::path path; digest tree;
  std::uint64_t records_fetched, bytes_remote;
};
struct unmount_request {
  std::optional<digest> tree;
  std::optional<std::filesystem::path> path;
};
struct prefetch_request { digest tree; };
struct node_store_mount {
  digest tree; std::filesystem::path path; std::uint32_t references;
  std::uint64_t chunks_fetched, bytes_fetched, bytes_avoided, hydrated_bytes;
};
struct node_store_status {
  std::uint64_t store_bytes, budget_bytes, evictions;
  std::vector<node_store_mount> mounts;
  std::optional<std::string> last_error;
};
runtime::task<void> serve_node_store(node_store_options);
```

The Unix listener alone exposes mount lifecycle calls. The TCP control listener exposes status,
attestation bytes, budget and eviction operations, OpenAPI, metrics, and MCP. See
[`node-store.md`](node-store.md) and [`../api/node-store.md`](../api/node-store.md).

### `liquid/store.hpp`

```cpp
class blob_write_through {
 public:
  virtual runtime::task<void> put(digest content, std::span<const std::byte> bytes,
                                  std::span<const digest> chunks,
                                  std::span<const std::uint64_t> chunk_sizes) = 0;
};
using hydration_observer = std::function<runtime::task<void>(
    digest content, digest chunk, std::uint64_t bytes, bool content_complete)>;
using remote_access_observer = std::function<void(digest content)>;
struct store_options {
  algorithm hash_algorithm{algorithm::blake3};
  std::uint64_t pack_size{};
  std::shared_ptr<keyring> keys;       // null preserves plaintext-store behaviour
  std::string write_key;               // empty selects keyring::default_key_id()
  bool durable{};
  std::map<std::string, std::shared_ptr<remote_blob_source>> remote_sources;
  std::shared_ptr<blob_write_through> write_through;
  hydration_observer on_hydration;
  remote_access_observer on_remote_access;
};
struct object_key_version { std::string key_id; std::uint64_t version; bool readable; };
struct collection_options {
  bool dry_run; std::chrono::seconds min_lease_age; bool explain;
  double compact_below{0.5}; bool compact_only;
};
struct collection_report {
  std::uint64_t roots, live_objects, live_chunks, dead_chunks, bytes_reclaimed;
  std::uint64_t bytes_reclaimable_in_partially_dead_packs, packs_deleted, epoch;
  std::vector<std::string> unknown_kinds;
  std::vector<partial_pack> partially_dead_packs;
  std::vector<retained_chunk> retained_chunks;
  std::vector<retained_chunk> retained_objects;
  std::map<std::string, std::uint64_t> retained_by_rule;
  std::uint64_t packs_compacted, bytes_reclaimed_by_compaction, bytes_written_by_compaction;
};
class lease { public: runtime::task<void> renew(std::chrono::seconds ttl);
                       runtime::task<void> release(); };
class store {                          // the chunk store: chunks, packs, liquid's own index
 public:
  static runtime::task<store> open(std::filesystem::path, store_options = {});
  runtime::task<digest> put(std::vector<std::byte>);              // chunked, deduplicated, indexed
  runtime::task<void> register_remote_source(
      std::string, std::shared_ptr<remote_blob_source>);          // add/replace after open
  runtime::task<std::uint64_t> local_bytes();                     // live unique raw chunk bytes
  runtime::task<digest> put_file(std::filesystem::path);          // streams from disk; a 40 GB file never sits in memory
  runtime::task<void> get_to_file(digest, std::filesystem::path); // streams to disk
  runtime::task<std::vector<std::byte>> get(digest);              // verified on read
  runtime::task<std::uint64_t> read(digest, std::uint64_t offset, std::span<std::byte> into);
  runtime::task<bool> contains(digest);
  runtime::task<blob_availability> has(digest);
  runtime::task<void> register_remote(blob);
  runtime::task<void> evict(digest);
  std::vector<digest> attestations(digest subject) const;
  runtime::task<std::optional<object_key_version>> key_version(digest);
  runtime::task<lease> lease_root(digest, std::chrono::seconds ttl);   // GC roots are leases
  runtime::task<collection_report> collect(collection_options = {});   // epoch marking
};
```

Chunking (FastCDC), packs, compression, the index and its tuning are internal; none of them
appears in a signature. File I/O is Corosio's `random_access_file` and `stream_file` on the
io_context's worker pool, behind the private boundary. `store_options` names the plaintext hash
algorithm, pack size, optional keyring, optional write key id, optional durable pack-write
acknowledgement, remote source map, and optional write-through callback. `store::evict` returns
`errc::not_remote` unless the whole-content digest has a persisted remote registration. A non-null
keyring enables trust-domain encrypted objects; see
[encryption.md](encryption.md).

### `liquid/remote.hpp`

```cpp
struct remote_blob_location { std::string source, instance; };
enum class remote_backend { reapi, liquid_scheduler };
struct remote_config { /* endpoint, instance, backend, TLS, headers, deadline, batch limit, publish */ };
class remote_blob_source {
 public:
  virtual runtime::task<std::vector<digest>> find_missing(std::string, std::vector<digest>) = 0;
  virtual runtime::task<std::uint64_t> read_range(std::string, digest, std::uint64_t,
                                                  std::span<std::byte>) = 0;
  virtual runtime::task<void> write(std::string, digest, std::vector<std::byte>) = 0;
};
class remote_executor {
 public:
  virtual remote_blob_source& blobs() noexcept = 0;
  virtual std::string instance() const = 0;
  virtual std::uint64_t queue_depth() const noexcept = 0;
  virtual bool supports_batch() const noexcept = 0;
  virtual runtime::task<action_result> execute(action_input) = 0;
  virtual runtime::task<action_result> execute_prepared(action_input, digest action,
                                                         digest sandbox,
                                                         execution_schedule);
  virtual runtime::task<std::vector<action_result>> execute_batch(std::vector<action_input>) = 0;
};
class remote {
 public:
  static runtime::task<remote> connect(remote_config);
  remote_blob_source& blobs() noexcept;
  remote_executor& executor() noexcept;
  runtime::task<std::shared_ptr<action_result>> get_action_result(repository&, action_input, digest);
  runtime::task<void> update_action_result(repository&, action_input, digest, const action_result&);
};
```

The interfaces remain transport-neutral; the store and VFS depend only on range reads and digest
verification. `remote` is their REAPI implementation. See [`reapi.md`](reapi.md).

### `liquid/reapi.hpp`

```cpp
struct reapi_serve_options { std::filesystem::path store_directory; std::string listen, instance_name; };
runtime::task<void> serve_reapi(reapi_serve_options);
```

The server exposes the repository's SHA-256 CAS, ByteStream, Capabilities, and ActionCache. It does
not expose transport implementation types.

### `liquid/sync.hpp`

```cpp
enum class sync_item_kind : std::uint8_t { drip = 1, change = 2, directory = 3, reference = 4 };
struct sync_item;       // signed actor/body, repository and directory IDs, root marker, ref value
struct sync_export { journal_cursor cursor; std::size_t count; bool done; std::vector<digest> objects; };
enum class sync_ref_outcome_kind : std::uint8_t { applied = 1, refused = 2 };
struct sync_ref_outcome { std::string name; sync_ref_outcome_kind outcome; std::string reason; };
struct sync_report {
  journal_cursor cursor; std::uint64_t metadata_items; sync_transfer_report sent, received;
  std::vector<sync_ref_outcome> references; std::uint64_t retry_after_ms;
  std::string exhausted_scope;
};
struct sync_metadata_report { std::uint64_t trees, blobs, bytes_remote; };
struct sync_endpoint_options { /* token, cursor, batch size */ bool atomic; };
struct sync_ref_update {
  std::string name; std::optional<change_id> old_head; change_id new_head; bool fast_forward;
};
enum class sync_ref_verdict_kind : std::uint8_t { accepted = 1, refused = 2 };
struct sync_ref_verdict { std::string name; sync_ref_verdict_kind verdict; std::string reason; };
using sync_authorizer = std::function<runtime::task<std::vector<sync_ref_verdict>>(
    const sync_item&, std::span<const sync_ref_update>)>;
struct sync_server_options { sync_authorizer authorize; std::size_t batch_items{256}; };
class sync_server {
 public:
  explicit sync_server(repository&, sync_server_options = {});
  sync_server(repository&, store&, sync_server_options = {});
  runtime::task<std::vector<std::byte>> handle_push(std::span<const std::byte>);
  runtime::task<std::vector<std::byte>> handle_pull(std::span<const std::byte>);
  runtime::task<std::vector<std::byte>> handle_find_missing(std::span<const std::byte>);
  runtime::task<void> handle_chunks(std::span<const std::byte>);
  runtime::task<std::uint64_t> handle_chunks(digest, std::uint64_t, std::span<std::byte>);
  static result<std::vector<std::byte>> encode_error(const error&);
};
class sync_session {
 public:
  runtime::task<sync_transfer_report> send(std::span<const digest> roots);
  runtime::task<sync_transfer_report> receive(std::span<const digest> roots);
  runtime::task<sync_metadata_report> receive_metadata(std::span<const digest> roots);
};
class sync_endpoint final : public remote_blob_source {
 public:
  explicit sync_endpoint(std::string repository_url, sync_endpoint_options = {});
  runtime::task<sync_report> push(repository&);
  runtime::task<sync_report> pull(repository&);
};
```

`sync_server` verifies that an explicitly supplied store belongs to the repository. Its optional
authorization task runs in metadata-frame order for changes and refs before import. Change calls
carry the ref updates that newly reach them; ref calls carry the server-known old head, proposed
new head, and a fast-forward value computed from the change graph. The task returns one named
verdict per update. The server applies accepted refs, returns each outcome through `sync_report`,
and makes the set all-or-nothing when endpoint options request `atomic`. Transport adapters call
the byte-span handlers and do not include private codec headers.
`repository::export_drips(cursor, span)` emits bounded pages of journal records, signed changes,
upstream directory sync messages, refs, and referenced object roots. `import_drips(span)` verifies
and applies the entire span against candidates before one checkpoint. The endpoint uses the protocol
in [sync.md](sync.md); pack frames remain private wire data.
`receive_metadata` accepts tree roots, stores only tree and blob records, registers blob content at
the session's remote source and instance, and reports fetched record counts plus bytes still remote.

### `liquid/delta.hpp`

```cpp
enum class delta_algorithm : std::uint8_t { zstd_patch = 1, executable_sections = 2 };
struct delta_record { digest source, target; delta_algorithm algorithm; digest blob; };
struct delta_creation {
  delta_record value; digest record, attestation;
  std::uint64_t source_bytes, target_bytes, delta_bytes;
};
enum class delta_fetch_method : std::uint8_t { delta, chunks };
struct delta_fetch_result {
  delta_fetch_method method; digest source, target;
  std::optional<digest> record, attestation;
};
result<std::vector<std::byte>> make_delta(delta_algorithm, source_bytes, target_bytes);
result<std::vector<std::byte>> apply_delta(const delta_record&, source_bytes, delta_bytes);
runtime::task<delta_creation> create_delta(store&, source_path, target_path,
                                           delta_algorithm = delta_algorithm::zstd_patch);
runtime::task<void> apply_delta(store&, digest record, source_path, destination_path);
runtime::task<delta_fetch_result> fetch_delta(store&, digest target, source_path, destination_path);
```

The codec is synchronous CPU work. Store/file operations are Capy tasks. `apply_delta` checks the
source, patch-blob, and target digests; `fetch_delta` checks the final target on delta and chunk
paths. The signed target catalog is local; its patch and fallback target may hydrate through a
registered `remote_blob_source` as defined by #17. Algorithm 2 is reserved and currently returns
`errc::unsupported`; [distribution.md](distribution.md#executable-aware-algorithm-reservation)
records the measurement it must beat.

The records' shapes and the metadata document are designed in [`object-model.md`](object-model.md).
Lease destruction has no storage effect; expiry is the authority. Collection reports are plain
values, and [gc.md](gc.md) defines their epoch and crash semantics.

### `liquid/abi.hpp`

```cpp
enum class abi_binary_kind : std::uint8_t { elf, macho, pe };
enum class abi_symbol_kind : std::uint8_t { function, object };
struct abi_symbol {
  std::string name;
  std::optional<std::string> version;
  abi_symbol_kind kind;
  std::uint64_t size;
  std::string binding;
  std::string visibility;
  std::optional<std::string> signature;
};
struct abi_descriptor {
  digest subject;
  abi_binary_kind binary;
  std::string architecture;
  std::optional<std::string> install_name;
  std::vector<abi_symbol> exported_symbols;
  std::vector<std::string> needed_libraries;
};
enum class abi_verdict : std::uint8_t { compatible, compatible_additions, incompatible };
enum class abi_change_kind : std::uint8_t {
  added, removed, type_changed, size_changed, version_changed
};
struct abi_change {
  std::string symbol;
  abi_change_kind kind;
  std::string detail;
};
struct abi_report {
  abi_verdict verdict;
  std::vector<abi_change> changes;
};
result<abi_descriptor> extract_abi(std::span<const std::byte> binary);
abi_report diff_abi(const abi_descriptor&, const abi_descriptor&);
result<std::string> abi_json(const abi_descriptor&);
result<std::string> abi_json(const abi_report&);
```

`repository::abi(digest library_blob)` asynchronously retrieves the stored descriptor for an
output blob. Extraction and diffing remain synchronous so callers can place CPU work according to
their runtime; result publication uses `runtime::offload`. The canonical record and verdict rules
are in [abi.md](abi.md).

### `liquid/attestation.hpp`

```cpp
enum class attestation_subject_kind : std::uint8_t { actor, drip, change, artifact, model, library };
enum class attestation_field_type : std::uint8_t { text, unsigned_integer, boolean, text_list };
enum class attestation_status_kind : std::uint8_t { none, max_age, revocable };
using attestation_value = std::variant<std::string, std::uint64_t, bool, std::vector<std::string>>;
struct attestation_attribute { std::string name; attestation_value value; };
struct attribute_disclosure_commitment { std::string name; std::vector<std::byte> value; };
struct slsa_provenance_v1 { slsa_build_definition build_definition; slsa_run_details run_details; };
struct attestation {
  digest subject; std::string profile; /* legacy scan fields and generic attributes */
  std::optional<slsa_provenance_v1> slsa_provenance;
};
class attestation_profile_registry { public: static const attestation_profile_registry& global();
  const std::vector<attestation_profile>& profiles() const; const attestation_profile* find(std::string_view) const;
  result<void> validate(const attestation&) const; };
result<std::vector<std::byte>> attribute_commitment(const attestation_attribute&, std::span<const std::byte> salt);
result<std::vector<std::byte>> encode_attestation_signature(const signature&);
result<std::vector<std::byte>> encode_attestation(const attestation&);
result<attestation> decode_attestation(std::span<const std::byte>);
result<attestation_verification> verify_attestation(const attestation&, const attestation_verification_options& = {});
```

`verify_attestation` rejects unknown profiles and validates the issuer signature, exact profile
schema, subject kind, registered freshness rule, revocation status evidence, and disclosed
commitments. [attestations.md](attestations.md) is the complete schema and wire-format guide.

### `liquid/object.hpp`

```cpp
enum class node_kind : std::uint8_t { blob, tree, change, action, result_, attestation, repo };
struct blob   { std::vector<digest> chunks; std::uint64_t size;
                std::vector<std::uint64_t> chunk_sizes; digest content;
                std::optional<remote_blob_location> remote; };
struct tree_entry { std::string name; std::uint32_t mode; node_kind kind; digest id; };
struct tree   { std::vector<tree_entry> entries; };                   // sorted by name
struct change { change_id id; std::vector<change_id> parents; actor author;
                std::string message; std::chrono::sys_seconds at; digest root; };
struct change_id { digest value; };    // stable across rebase and transplant (features/02)
```

`chunk_sizes` is appended to the blob value and canonical record so a range reader can locate the
covering logical chunks without reading every preceding chunk. The decoder accepts the earlier
single-chunk record and derives its one size, preserving repositories written before liquid #6.

Attestation values and every registered profile are exposed by `liquid/attestation.hpp`.
`action` and `result_` are implemented by the Phase 2 build engine; `repo` is implemented in Phase
1. Two fields are reserved
from the first commit because they
cannot be added later: an optional `symbols` reference on built artifacts (liquid #47) and an
optional `conflict` record on a change (liquid #40).

### `liquid/repository.hpp`

```cpp
struct key_readability { std::string key_id; std::uint64_t version; bool readable; };
struct revision_readability { change_id revision; std::vector<key_readability> keys; };
struct readability_report { std::vector<revision_readability> revisions;
                            std::vector<std::string> limits; };
enum class tree_diff_kind { added, removed, modified, moved, mode_changed, conflicted };
struct tree_diff_options { bool text; };
struct tree_diff_entry {
  std::string path; std::optional<std::string> old_path; node_id node; node_kind kind;
  std::vector<tree_diff_kind> changes; std::optional<std::uint32_t> old_mode, new_mode;
  std::optional<digest> old_blob, new_blob; std::optional<std::vector<line_change>> line_hunks;
  std::vector<digest> old_conflicts, new_conflicts;
};
struct text_anchor { change_id change; std::string path; std::uint64_t line, column; };
struct mapped_anchor { change_id change; std::string path; std::uint64_t line, column;
                       bool moved, approximate, deleted; };
struct record_cursor { std::string token; };
struct record_page_options { std::optional<record_cursor> cursor; std::uint64_t limit{100}; };
template<class T> struct record_page { std::vector<T> records; std::optional<record_cursor> next;
                                      bool done; };
struct repository_result { result_id id; action_result value; std::uint64_t sequence;
                           bool divergent; std::optional<digest> verification; };
struct result_filter { std::optional<result_id> id; std::optional<digest> action;
                       std::optional<std::string> output; std::optional<digest> content;
                       record_page_options page; };
struct repository_attestation { digest id; attestation value; };
struct attestation_filter { std::optional<digest> id, subject; std::optional<std::string> profile, issuer;
                            record_page_options page; };
struct search_options { std::optional<std::string> ref; std::uint64_t limit{20}; };
enum class search_executor { bm25f, trigram, scan };
enum class search_query_kind { ranked_text, literal, regex_with_literal_core,
                               regex_without_literal_core };
struct search_hit { std::string path; digest blob; std::uint64_t line, column;
                    std::string snippet; std::optional<double> score; };
struct search_alternative { search_executor executor; std::uint64_t estimated_cost_us;
                            std::optional<std::uint64_t> actual_cost_us; bool eligible;
                            std::string reason; };
struct search_plan { search_executor executor; search_query_kind query_kind;
                     std::uint64_t estimated_cost_us, actual_cost_us;
                     std::uint64_t working_set_blobs, working_set_bytes,
                                   drips_since_index_update, scan_nanoseconds_per_blob,
                                   scan_parallelism;
                     double trigram_selectivity; std::vector<search_alternative> alternatives; };
class repository {
 public:
  static runtime::task<repository> create(std::filesystem::path, repository_options = {});
  static runtime::task<repository> open(std::filesystem::path, repository_options = {});
  static runtime::task<repository> open_build(std::filesystem::path working_tree,
                                               repository_options = {});
  runtime::task<tree> root();                            // the working copy is a change
  runtime::task<change_id> head();
  runtime::task<std::vector<std::byte>> read(std::string path);
  runtime::task<std::vector<std::byte>> read_decrypted(std::string path);  // explicit leaf/file decryption
  runtime::task<std::vector<search_hit>> search(std::string query, search_options = {});
  runtime::task<search_plan> plan_search(std::string query, search_options = {});
  runtime::task<void> write(std::string path, std::vector<std::byte>);   // becomes a drip
  runtime::task<void> redact(std::string path, std::optional<change_id> = {});
  runtime::task<void> redact(std::string path, std::vector<std::byte> replacement,
                             std::optional<change_id> = {});
  runtime::task<std::vector<std::string>> diff_keys(std::string path, change_id a, change_id b);
  runtime::task<void> write_file(std::string path, std::filesystem::path source,
                                 write_options = {});   // streams a large file in; one drip
  runtime::task<void> write_remote(std::string path, blob, write_options = {});
  runtime::task<digest> store_remote_blob(blob);
  runtime::task<void> move(std::string from, std::string to, write_options = {});
  runtime::task<void> remove(std::string path, write_options = {});
  runtime::task<change_id> cut(std::string message);      // name the current change; the working copy starts a new one
  runtime::task<std::vector<ref>> refs();                 // name → change_id
  runtime::task<void> set_ref(std::string name, change_id);
  result<repository_status> status();                     // includes bytes still remote in this tree
  runtime::task<std::vector<change>> log(log_options = {});
  runtime::task<snapshot> at(change_id);                  // historical tree
  runtime::task<document_state> document_at(change_id);   // historical refs, locks, and nesting
  runtime::task<std::vector<drip>> drips();
  runtime::task<stale_base_report> explain_stale(write_base, std::vector<std::string> paths);
  runtime::task<std::vector<tree_diff_entry>> diff(change_id old, change_id next,
                                                    tree_diff_options = {});
  runtime::task<std::vector<tree_diff_entry>> diff(digest old_tree, digest next_tree,
                                                    tree_diff_options = {});
  runtime::task<std::vector<tree_diff_entry>> diff(tree old, tree next,
                                                    tree_diff_options = {});
  runtime::task<mapped_anchor> map_anchor(text_anchor, change_id target);
  runtime::task<std::vector<mapped_anchor>> map_anchors(std::span<const text_anchor>,
                                                        change_id target);
  runtime::task<record_page<repository_result>> results(result_filter = {});
  runtime::task<action_input> action(digest); // action or result record
  runtime::task<record_page<repository_attestation>> attestations(attestation_filter = {});
  runtime::task<record_page<repository_attestation>> attestations(digest subject,
                                                                   record_page_options = {});
  runtime::task<record_page<build_trace>> traces(record_page_options = {});
  runtime::task<record_page<sbom_report>> sboms(result_id, record_page_options = {});
  runtime::task<abi_descriptor> abi(digest library_blob);
  runtime::task<readability_report> readability(std::optional<change_id> = {});
  runtime::task<void> nest(repository source, std::string path);
  runtime::task<repository> split(std::string path, std::filesystem::path into);
  runtime::task<void> combine(repository other, std::optional<std::string> at);
  runtime::task<std::vector<conflict>> conflicts();      // #40: conflicts are values
  runtime::task<std::vector<operation_record>> operations(log_options = {}); // newest first
  runtime::task<undo_report> undo(op_id);
  runtime::task<undo_report> undo_session(session_id, undo_options = {});
  runtime::task<void> lock(std::string path);            // #42
  runtime::task<void> unlock(std::string path);
  runtime::task<digest> store_attestation(attestation);    // store canonical evidence; no trust implied
  runtime::task<collection_report> collect(collection_options = {}); // checkpoints and collects
  runtime::task<retention_explanation> retain_explain(digest);       // live rule/lease or eviction
  runtime::task<cache_report> cache_stats();               // hits, misses, bytes, critical alerts
  runtime::task<digest> root_digest();                     // current tree record
  runtime::task<std::vector<build_plan_action>> build_plan(build_target = {});
  result<build_handle> build(build_target = {});           // configured repository build
  result<build_handle> build(action_input);
  result<build_handle> build(action_input, remote_executor&);
  runtime::task<void> register_test(test_target);           // native definition hook
  runtime::task<std::vector<test_target>> tests();
  result<build_handle> run_tests(std::string filter = {});
  runtime::task<void> set_policy(policy_config);           // validate and checkpoint config.policy
  runtime::task<sync_export> export_drips(journal_cursor, std::span<sync_item>);
  runtime::task<sync_import_report> import_drips(std::span<const sync_item>);
  flow_config flow_configuration() const;
  std::vector<flow_bucket_status> flow_buckets() const;
  // Import primitives (liquid #43; lake's importers use the same calls)
  runtime::task<change_id> record(change_input);                 // a change with explicit parents, author, time, message, metadata, ref
  runtime::task<change_id> record_from_tree(change_input, digest root_tree); // records a supplied snapshot tree
  runtime::task<void> set_metadata(std::string key, std::vector<std::byte>);   // repository-document metadata map
  runtime::task<std::optional<std::vector<std::byte>>> metadata(std::string key);
  runtime::task<std::vector<std::string>> metadata_keys(std::string prefix);
  runtime::task<tree> tree(digest);                              // a tree record from the store (historical snapshots)
  runtime::task<std::vector<std::byte>> blob(digest);            // a blob's bytes by digest
  runtime::task<void> write(std::string path, std::vector<std::byte>, write_options);
  runtime::task<void> nest_ref(std::string path, repo_ref);      // a nested-repository placeholder without the repository present
};
struct change_input { std::vector<change_ref> parents; actor author; std::chrono::sys_seconds at; std::string message;
                      std::map<std::string, std::string> metadata; std::optional<std::string> ref; };
struct agents_config { bool require_base; std::vector<std::string> actors; };
struct write_base { change_id change; std::optional<journal_cursor> cursor; };
struct write_options {
  enum class stale_action : std::uint8_t { refuse, accept_if_disjoint, force };
  std::uint32_t mode{0100644};
  std::optional<write_base> base;
  std::vector<std::string> depends_on;
  stale_action on_stale{stale_action::refuse};
};
struct stale_path_change { std::string path; std::vector<line_change> hunks; };
struct stale_base_report {
  write_base base; change_id head; std::vector<stale_path_change> changed;
  std::uint64_t since_drips;
};
// log_options gains: std::optional<change_id> from; bool first_parent{false};  (full DAG, topological order, by default)
```

`repository_options::encrypt.paths` is an ordered vector of `{ path, key_id, mode,
encrypted_regex }`. The first repository-relative glob match selects leaf or file mode. Leaf mode
is implemented for JSON and `.env`; [leaf-encryption.md](leaf-encryption.md) defines its envelope,
normalization, enforcement, and JSON Pointer key paths.

`search_options` selects the working tree by default or a named ref and limits results to 20 by
default. Plain queries use BM25F; quoted and slash-delimited queries let the measured planner choose
verified trigrams or a current mapped scan. `search_hit` carries the repository-relative path,
content digest, one-based line and column, one-line snippet, and a score only for ranked results.
`plan_search` executes the query and returns the same choice with estimates, actual cost, measured
inputs, and alternatives that preserve or reject the requested result set. Publication, freshness,
and the private index format are documented in [search.md](search.md).

`ref` is `{ std::string name; change_id id; }`; `repository_status` is a plain record: `pending_drips`, `paused`,
`conflicts`, `oversized_directories` (paths with more than 10,000 entries), `head`, and `agents`.
`repository_options` is a plain record with `checkpoint_interval`, caller-owned `store`, local
`storage`, `encrypt`, `cache`, `verify`, `agents`, `policy`, `retention`, and `flow`. The persisted
`agents` record has `require_base` and a list of TAAP actor identifiers; the base requirement
applies only to listed actors. `storage` selects the
object-store hash algorithm and key custody when `store` is null. When `store` points to an open
caller-owned store, that store's configuration is authoritative and the caller keeps it alive for
the repository's lifetime. Pass the same placement to `create` and `open` so later writes and reads
use the intended store.
Repository change IDs are computed with that selected store algorithm, including IDs returned by
`log` and accepted by `at`.

`cache_config` is `{ bool publish{}; }`. Its read-only default allows lookup but refuses action-index
publication with `errc::unauthorized`; the value is persisted at repository creation.
`verify_config` is `{ std::uint32_t rate{20}; }`; zero is invalid and the rate is persisted at
repository creation. `cache_report` contains `hits`, `misses`, `bytes_served`, `bytes_stored`, and
repository creation. `cache_report` contains aggregate hit, miss, served, stored, and rebuilt bytes;
miss-reason counts; memory, disk, and remote tier rows; and critical reproducibility alerts for the
open repository handle. `action_result::cache` is `cache_status::hit`, `cache_status::miss`, or
`cache_status::divergent`; on a validation miss, `why` contains the first differing path and optional
expected/actual digests. These fields are request diagnostics rather than part of the signed immutable
result record.

`policy_config` is the typed build-input rule set documented in [policy.md](policy.md). It is
persisted at creation and replaced atomically through `repository::set_policy`; validation rejects
unknown profiles or impossible quorums before the repository document changes.
`repository::store_attestation` imports a canonical attestation into the repository object store
and returns its digest. Storage does not imply trust: policy reloads the record and verifies its
signature and registered profile each time it is evaluated.

`retention_config::rules` holds `ref_retention_rule`, `result_retention_rule`, and
`size_cap_retention_rule` variants. Ref rules have a name, glob, and optional duration (no duration
means forever); result rules have a base duration scaled by recorded execution cost; a size-cap
 rule carries physical store bytes. At most one result rule and one size-cap rule are valid. The
 configuration is persisted in the repository document and `retain_explain` reports the computed
 outcome.

`key_readability` is `{ key_id, version, readable }`. `revision_readability` is a `change_id` and
the sorted key-version set used by its blobs. `readability_report` contains those revision rows and
the three required rotation, revocation, and metadata-disclosure statements.

`session_id` is a printable 16-byte identifier. `repository_options::session` fixes it for an
opened handle; an omitted value generates a random identifier per handle. `operation_record` is
`{ op_id id, std::string kind, std::string from, std::string to, actor by,
std::chrono::sys_seconds at, session_id session }`. Each operation ID is unique, so it selects one
operation for `undo`. `operations()` returns the repository document's recorded operations newest
first. `to_string(session_id)` and `parse_session_id` use 32 lowercase hexadecimal digits.

`undo_report` has ordered `applied` operation IDs and `skipped` entries of `{ op, path, reason }`.
An inverse applies only where current state still matches that operation's recorded after-state.
`undo_session` reverses matching operations newest first and accepts optional path and drip-ID
filters through `undo_options`; each affected directory is committed as one transaction and no
candidate document is published when planning or a transaction fails.

`document_state` contains the reference map, lock paths, and `{ path, repo_ref }` nested pointers
recorded in a version 2 change. `document_at` validates that change's directory heads through
full-history archives and never reads from or mutates the active shallow documents. Version 1
change records continue to support `at`, but have no document projection.

`repository::collect` checkpoints the working tree, retains leases for that tree, every current ref
head, the checkpoint document, and its lazy directory documents, then delegates to
`store::collect`. Repository callers use this operation instead of collecting the repository's store
directly.
Existing repository documents without the added operation metadata remain readable; their records
return an empty actor, epoch timestamp, and zero session. Their legacy operations cannot be undone
when no inverse effects were recorded.

Tree diffs match paths by `node_id`, expose independent change kinds plus both blob and mode sides,
and read UTF-8 blob contents only when `tree_diff_options::text` requests `line_change` hunks.
Change-level diffs add the exact conflict values from both records. The tree value and tree-digest
forms omit conflicts because no change record is available. Phase 4 may append a structural diff
field without replacing the existing byte/line fields.

Text anchors use one-based lines and columns. Mapping follows `node_id` moves and applies one
change-pair diff's line hunks: unchanged lines retain their columns, earlier insertions and removals
shift later lines, replacement lines are approximate, and removed regions are deleted. `moved`
means the returned path or line changed. A batch must share one source change, preserves caller
order, scans diff entries once, and walks each anchored path's hunks once after sorting that path's
anchors. Modified non-text blobs return `errc::unsupported`.

Record pages use opaque stored-record digest cursors. Limits are 1 through 1,000. Result filters
cover result id, action id, output path, and output content; result values retain sequence,
divergence, and verification fields from the action index when present. Attestation filters cover
record id, subject, profile, and issuer. Trace and result-scoped SBOM pages return `build_trace` and
verified `sbom_report` values from `build.hpp`. The CLI record views call these operations.

The internal CRDT wrapper pins automerge-cpp at `06408d3`. Directory checkpoints call
`save_compact()`, reopen through `load_shallow()`, and replay later journaled drips. Local
automerge validation failures map to `errc::invalid_argument`; malformed checkpoint, merge, and
sync data map to `errc::corrupt`. Apply, merge, and sync-receive expected values are checked before
candidate state is installed, and remote observation uses the patch-returning entry points. The
wrapper exposes no automerge type in this public header.

### `liquid/vfs.hpp`

```cpp
struct mount_options { bool read_only{false}; std::chrono::seconds attr_timeout{1}; };
struct mount_status { std::uint64_t hydrated_bytes, hydrated_files, misses_served,
                      writes_captured; std::chrono::nanoseconds request_queue_latency; };
class mount {
 public:
  runtime::task<void> unmount();
  result<mount_status> status() const;
};
namespace vfs {
runtime::task<::liquid::mount> mount(repository&, std::filesystem::path, mount_options = {});
runtime::task<void> materialise(repository&, std::filesystem::path,
                                std::optional<std::string> path_prefix = std::nullopt);
}
```

The handle contains no FUSE type. Backend details and the persistent hydration cache are private
under `src/vfs/`; see [`vfs.md`](vfs.md).

### `liquid/build.hpp`

```cpp
enum class cache_status : std::uint8_t { miss, hit, divergent };
enum class cache_miss_reason { input_changed, command_changed, environment_changed,
  toolchain_changed, sandbox_changed, never_built, divergent_result_quarantined, unknown,
  policy_refused, non_hermetic_read };
struct action_input { /* command, roots, platform, timeout */ bool remote_eligible; };
enum class build_event_kind { action_queued, action_started, action_hit, action_done,
  action_failed, critical_path_updated, test_result, build_finished };
enum class action_execution_kind { local, remote };
struct action_execution { action_execution_kind kind; std::string worker; };
enum class test_status { passed, failed };
struct test_result { std::string name; test_status status; std::string output;
                     std::chrono::milliseconds duration; bool hit; };
struct test_target { std::string name; std::vector<std::string> command, labels, inputs;
                     std::string cwd; digest toolchain; liquid::platform platform;
                     std::optional<std::chrono::seconds> timeout; };
struct build_target { std::string name; }; // empty means the whole configured build
struct build_plan_action { std::uint64_t v; digest action; action_input input;
  std::vector<digest> dependencies; cache_status state; std::optional<result_id> result; };
result<std::string> plan_json(const build_plan_action&);
struct build_summary { int exit; std::uint64_t actions, hits, failures;
                       std::chrono::milliseconds duration; };
struct build_event { build_event_kind kind; std::uint64_t sequence;
  std::optional<digest> action; std::optional<result_id> result;
  std::optional<action_execution> execution; std::vector<action_output> outputs;
  std::vector<frontend_diagnostic> diagnostics; std::vector<critical_action> critical_path;
  std::optional<test_result> test; std::optional<build_summary> summary; };
class build_event_subscription { public: runtime::task<std::optional<build_event>> next(); };
class build_handle { public: result<build_event_subscription> events() const;
                     runtime::task<build_summary> wait(); };
result<std::string> event_json(const build_event&);
struct cache_miss { cache_miss_reason reason; std::string path;
  std::optional<digest> expected, actual; /* argv, environment, tool, line, attribution, detail */ };
enum class policy_scope : std::uint8_t { paths, toolchain, all_inputs };
enum class policy_on_missing : std::uint8_t { refuse, warn };
struct policy_requirement { std::string profile; std::vector<std::string> from;
                            std::uint32_t min_count; };
struct policy_rule { policy_scope scope; std::vector<std::string> paths;
                     std::vector<policy_requirement> require; policy_on_missing on_missing; };
struct policy_config { std::vector<std::string> approved_builders;
                       std::vector<policy_rule> rules; };
struct policy_requirement_result { policy_requirement requirement;
                                   std::vector<std::string> identities;
                                   std::uint32_t count; bool satisfied; std::string reason; };
struct policy_input_result { std::string input; digest subject; bool toolchain;
                             std::uint64_t rule; policy_on_missing on_missing;
                             std::vector<policy_requirement_result> requirements; bool satisfied; };
struct policy_report { policy_config config; digest policy; bool allowed;
                       std::vector<policy_input_result> inputs;
                       std::vector<std::string> warnings; };
struct reproducibility_difference {
  std::string output; std::optional<digest> cached, reexecuted;
  std::optional<std::uint64_t> byte_offset;
  std::optional<std::uint8_t> cached_byte, reexecuted_byte;
};
struct critical_cache_alert {
  digest action, result; std::vector<reproducibility_difference> differences;
};
struct cache_report {
  std::uint64_t hits, misses, bytes_served, bytes_stored, bytes_rebuilt;
  std::map<cache_miss_reason, std::uint64_t> misses_by_reason;
  cache_tier_report memory, disk, remote;
  std::vector<critical_cache_alert> critical_alerts;
};
enum class action_size_class : std::uint8_t { unknown, tiny, short_action, medium, long_action };
enum class execution_decision : std::uint8_t { local, remote, race };
struct scheduling_inputs {
  std::vector<action_timing> history; action_size_class size_class; std::uint64_t input_bytes;
  bool remote_state_queried; std::uint64_t remote_missing_bytes, remote_queue_depth;
  std::uint32_t remote_cache_locality_permille; bool batch_supported; std::uint64_t batch_size;
};
struct execution_schedule {
  execution_decision decision; scheduling_inputs inputs;
  std::chrono::milliseconds local_estimate, remote_estimate; std::string reason;
};
struct action_result {
  digest action; int exit; std::vector<action_output> outputs; digest stdout_output, stderr_output;
  std::vector<observed_input> manifest; action_timing timing; std::string worker;
  std::chrono::sys_seconds observed_at; actor producer; std::vector<std::byte> sig;
  cache_status cache; std::optional<cache_miss> why;
  std::optional<std::chrono::sys_seconds> started_at, finished_at;
  std::optional<digest> provenance; std::uint64_t record_version;
  std::vector<digest> sboms; std::optional<execution_schedule> schedule;
};
struct build_trace_action { result_id result; digest action; std::vector<std::string> outputs;
  action_timing timing; cache_status cache; std::chrono::milliseconds start_offset;
  std::optional<execution_schedule> schedule; };
struct build_trace_edge { digest dependency, dependent; };
struct build_trace { digest id; std::chrono::milliseconds wall_time;
  std::vector<build_trace_action> actions; std::vector<build_trace_edge> edges; cache_report cache; };
struct why_slow_report { std::string target; digest trace; std::chrono::milliseconds duration;
  std::vector<critical_action> critical_path; };
struct provenance_report {
  result_id result; digest attestation; liquid::attestation statement; action_result value;
  bool verified; std::string reason;
};
struct fact_key { digest toolchain, sysroot; std::string target; };
struct fact_answer {
  int exit; std::vector<action_output> outputs;
  std::map<std::string, std::string> variables; std::optional<digest> result;
};
struct configure_fact { digest id; std::string probe; fact_key key; fact_answer answer; };
struct fact_bundle { digest id; std::vector<configure_fact> facts; };
enum class sbom_format : std::uint8_t { spdx, cyclonedx };
struct sbom_component {
  std::string id, name, kind; std::optional<digest> content;
  std::optional<std::string> repository_id; std::optional<digest> pinned_change; bool inferred;
};
struct sbom_relationship { std::string from, to, kind; };
struct sbom_report {
  digest subject, record, attestation; liquid::attestation statement; std::string document;
  bool derived, inferred, verified; std::string reason;
  std::vector<sbom_component> components; std::vector<sbom_relationship> relationships;
};
enum class verify_selection : std::uint8_t { sample, all, changed, action };
struct verify_options {
  verify_selection selection; std::optional<digest> action; bool explain;
  std::optional<std::filesystem::path> cache_directory;
};
struct verified_action {
  digest action, result, attestation; bool agreement;
  std::vector<reproducibility_difference> differences;
};
struct verify_report { std::uint64_t considered; std::vector<verified_action> actions; };
struct graph_verify_options {
  bool all, explain; std::uint32_t rate;
  std::optional<std::filesystem::path> cache_directory;
};
struct graph_verify_report { graph_validation_report graph; verify_report actions; };
struct replay_options {
  bool record; std::optional<std::filesystem::path> cache_directory;
};
struct replay_report {
  digest action, result; int recorded_exit, reexecuted_exit; bool agreement;
  std::vector<reproducibility_difference> differences; std::optional<digest> attestation;
};
namespace build {
result<build_handle> start(repository&, action_input);
result<build_handle> start(repository&, action_input, remote_executor&);
result<build_handle> start_wrap(std::filesystem::path, std::vector<std::string>,
                                wrap_options = {});
execution_schedule choose_execution(scheduling_inputs);
runtime::task<result_id> execute(repository&, action_input);
runtime::task<std::vector<std::byte>> execute_symbol_action(repository&, action_input,
                                                             std::vector<std::byte>);
runtime::task<result_id> record_remote_result(repository&, action_input, action_result);
runtime::task<result_id> execute(repository&, action_input, remote_executor&);
runtime::task<std::vector<result_id>> execute_batch(repository&, std::vector<action_input>,
                                                    remote_executor&);
runtime::task<action_result> result(repository&, result_id);
runtime::task<wrap_report> wrap(std::filesystem::path, std::vector<std::string>, wrap_options = {});
result<std::filesystem::path> wrap_cache_directory(const std::filesystem::path&);
runtime::task<verify_report> verify(repository&, verify_options = {});
runtime::task<verify_report> verify(std::filesystem::path, verify_options = {});
runtime::task<graph_verify_report> verify_graph(std::filesystem::path,
                                                graph_verify_options = {});
runtime::task<replay_report> replay(repository&, digest target, replay_options = {});
runtime::task<replay_report> replay(std::filesystem::path, digest target, replay_options = {});
runtime::task<provenance_report> verify_provenance(repository&, result_id);
runtime::task<provenance_report> verify_provenance(repository&, std::string output_path);
runtime::task<provenance_report> verify_wrapped_provenance(std::filesystem::path, std::filesystem::path);
result<std::string> provenance_json(const provenance_report&, bool dsse = false);
runtime::task<why_report> why(std::filesystem::path, std::string,
                              std::optional<std::filesystem::path> cache = {});
runtime::task<cache_report> cache_stats(std::filesystem::path,
                                        std::optional<std::filesystem::path> cache = {});
runtime::task<build_trace> latest_trace(std::filesystem::path,
                                        std::optional<std::filesystem::path> cache = {});
runtime::task<why_slow_report> why_slow(std::filesystem::path, std::string,
                                        std::optional<std::filesystem::path> cache = {});
liquid::result<std::string> chrome_trace(const build_trace&);
runtime::task<policy_report> check_policy(repository&, const action_input&);
runtime::task<policy_report> check_policy(repository&, result_id);
runtime::task<policy_report> check_policy(std::filesystem::path, std::string target);
result<policy_report> explain_policy(repository&);
runtime::task<policy_report> explain_policy(std::filesystem::path);
runtime::task<std::vector<configure_fact>> facts_list(std::optional<std::filesystem::path> = {});
runtime::task<configure_fact> facts_show(digest, std::optional<std::filesystem::path> = {});
runtime::task<std::string> facts_export(std::optional<std::filesystem::path> = {});
runtime::task<sbom_report> generate_sbom(repository&, result_id, std::string, sbom_format);
runtime::task<sbom_report> generate_sbom(repository&, std::string output_path, sbom_format);
runtime::task<sbom_report> generate_wrapped_sbom(std::filesystem::path, std::filesystem::path,
                                                 sbom_format);
runtime::task<sbom_report> infer_sbom(std::filesystem::path artifact, sbom_format);
}
```

The candidate key covers command, cwd, environment, toolchain, sandbox definition, platform, and
the canonical policy digest.
`execute` returns an indexed result only after the result's signed action derives that same key,
all observed paths match the current input tree, all referenced blobs exist, and the referenced
provenance and SBOMs verify. Producer, `sig`, timestamps, `provenance`, and `sboms` are immutable
record fields; cache status and miss detail describe the current request. The remote overload
ships source and toolchain object graphs through `remote_blob_source`, then records the
`choose_execution` inputs and local, remote, or race decision in the signed result. `execute_batch`
groups at least four recorded short actions when the executor advertises support. A race publishes
only the winner and cancels the other task. The interfaces have no REAPI, gRPC, protobuf, lake, or
worker implementation dependency. `verify` re-executes
selected cache entries with their recorded sandbox definition, compares exit status and output
digests, and returns the signed attestation digest plus any differences. A divergent index entry is
permanently excluded from lookup. `verify_graph` first cleanly recomputes the wrap graph's sampled
or complete `action_result` nodes, then runs the matching action-verification selection; its two
reports keep graph and action evidence distinct.
`replay` uses the same reconstruction, sandbox launch, and comparison for one action or result.
It returns both exit values and byte-level differences without an attestation by default. Setting
`replay_options::record` records the ordinary verification attestation and quarantine decision.
Remote blobs hydrate through the registered `remote_blob_source` identified by their recorded
`remote_blob_location`; the store verifies every fetched chunk before local storage. `write`
completes the transport-neutral CAS interface but replay never calls it or publishes its local
result. See
[build-engine.md](build-engine.md#reproducibility-verification) and
[provenance.md](provenance.md) for the SLSA mapping and interim object subject, and
[sbom.md](sbom.md) for the shared component model, signed record chain, and inference markers.
`why` accepts an output path, action digest, or result digest and returns the selected lookup plus
current/prior identifiers and a typed cause. It re-evaluates current policy before classifying a
hit or ordinary miss and uses `cache_miss_reason::policy_refused` for an unsatisfied refuse rule.
`wrap_report` includes action counts, its per-wrap `cache_report`, typed miss details, the retained
trace digest, phase totals, achieved parallelism, the dependency-aware critical path, graph
validation backlog, and the last graph divergence.
`why_report`, trace actions, critical-path actions, and Chrome events carry the stored schedule, so
diagnostics expose history, size class, byte counts, queue depth, locality, batching, estimates, and
reason without recalculating the choice. `latest_trace`, `why_slow`, and `chrome_trace` query and
project only recorded trace/result data.
The path-taking `cache_stats`
returns cumulative completed-wrap statistics; the repository member remains scoped to its open
handle.
`check_policy` returns a complete per-rule report without executing an action; build entry points
turn an unsatisfied `refuse` rule into `errc::unauthorized` before lookup or sandbox launch.
Configure facts are immutable content-addressed records. Their exact key is compiler content,
explicit sysroot-tree content, and target triple; lookup does not admit partial matches.
`facts_export` returns a `fact-bundle/1` JSON record for later transport layers. `wrap_options`
accepts optional target and fact-directory fields, and a foreign target never executes an uncached
probe.

### `liquid/drip.hpp` and `liquid/identity.hpp`

```cpp
enum class stale_revalidation : std::uint8_t { disjoint, forced };
struct drip { drip_id id; change_id base; std::vector<path_op> ops; actor by;
              std::vector<std::byte> signature; std::chrono::sys_seconds at;
              std::optional<write_base> reasoned_from;
              std::optional<stale_revalidation> revalidated;
              std::optional<digest> scan_attestation; std::optional<scan_block> block; bool redacted; };
struct scan_allow_rule { std::string path; std::vector<std::string> detectors; };
struct scan_configuration { std::vector<scan_allow_rule> allow;
                            std::string ignore_lines{"liquid:allow-secret"}; };
struct capture_options { std::vector<std::string> exclude;            // glob patterns from config
                         std::chrono::milliseconds coalesce_window{200};
                         bool scan{true}; scan_configuration scan_config;
                         bool attribution{false}; };                   // Phase 2 turns it on
struct journal_cursor { std::string token; };
struct capture_status { enum class state { running, paused } state; std::uint64_t pending_drips;
                        std::uint64_t overflow_count, recrawl_count, blocked_drips;
                        std::optional<scan_block> last_block; journal_cursor last_cursor; };
class capture {                        // the journal, as a subscription
 public:
  static runtime::task<capture> open(std::filesystem::path workspace, capture_options = {});   // starts the watcher
  runtime::task<void> close();
  result<subscription> subscribe(std::function<void(const drip&)>);   // registration is synchronous
  result<subscription> subscribe_from(journal_cursor,
      std::function<void(const drip&, const journal_cursor&)>);
  runtime::task<void> pause();  runtime::task<void> resume();  result<capture_status> status();
};
struct actor { std::string taap_id; std::string fingerprint; std::vector<std::byte> public_key;
               std::string key_fingerprint; };           // stable identity and current key identifiers
struct compromise_declaration { std::string key_fingerprint_base32; std::string declared_at;
                                std::optional<std::string> reason; };
struct identity_document { actor subject; std::string key_label; std::string created_at; std::string updated_at;
                           std::optional<std::vector<std::byte>> previous_document_hash;
                           std::vector<compromise_declaration> compromised_keys; std::vector<signature> signatures; };
struct verified_identity { actor subject; std::vector<actor> current_keys; bool has_compromised_history; };
result<std::vector<std::byte>> encode_identity_document(const identity_document&);
result<identity_document> decode_identity_document(std::span<const std::byte> encoded);
result<signature> decode_signature(std::span<const std::byte> encoded);
class signer {
 public:
  runtime::task<signature> sign(std::string artifact_type, std::vector<std::byte> canonical_body);  // TAAP/1 §6.6
  runtime::task<identity_document> rotate(std::optional<compromise_declaration> compromise = {});
};
result<bool> verify(const actor&, std::string_view artifact_type, std::span<const std::byte> canonical_body, const signature&);
result<verified_identity> verify_chain(std::span<const identity_document> documents);
struct signature { std::string algorithm; std::string signing_key_identifier; std::string signing_key_label;
                   std::string signature_purpose; std::string signed_at; std::vector<std::byte> bytes; };   // TAAP/1 §6.7
struct argon2_parameters { std::uint32_t memory_kib; std::uint32_t iterations; std::uint32_t lanes; };
struct rotation_report { std::string key_id; std::uint64_t previous_version, new_version;
                         std::vector<std::string> limits; };
class key_provider { protected: virtual result<std::array<std::byte, 32>> derive_kek(span, argon2_parameters) const = 0; };
class passphrase_provider final : public key_provider { public: result<std::array<std::byte, 32>> derive(span, argon2_parameters = {}) const; };
class key_file_provider final : public key_provider { public: static runtime::task<std::shared_ptr<key_file_provider>> open(std::filesystem::path); };
class keyring {
 public:
  result<void> add(std::string, std::shared_ptr<key_provider>);
  result<std::string> default_key_id() const;
  result<void> set_default_key_id(std::string);
  runtime::task<void> rewrap(std::string, std::shared_ptr<key_provider>);
  runtime::task<rotation_report> rotate(std::string);
  runtime::task<void> revoke(std::string, std::uint64_t);
};
```

### `liquid/daemon.hpp`

```cpp
struct daemon_delivery { drip value; journal_cursor cursor; };
class daemon_target {
 public:
  virtual ~daemon_target() = default;
  virtual std::string name() const = 0;
  virtual runtime::task<void> deliver(daemon_delivery) = 0;
};
struct daemon_target_status {
  std::string name; journal_cursor cursor; std::uint64_t behind, deliveries;
  std::optional<std::string> last_error;
};
struct daemon_status {
  bool running; std::uint64_t process_id; std::string endpoint;
  std::optional<drip_id> last_drip; std::vector<std::string> invalidated_paths;
  std::vector<daemon_target_status> targets; capture_status capture;
  std::uint64_t graph_keys, graph_values, graph_memory_bytes, graph_memory_ceiling;
  std::uint64_t graph_validation_backlog;
  std::optional<graph_validation_record> last_graph_divergence;
  flow_status flow;
};
struct daemon_options {
  std::chrono::milliseconds idle_timeout; std::uint64_t graph_memory;
  std::uint32_t graph_validation_rate; std::chrono::milliseconds graph_validation_interval;
  query_registry graph_queries;
  capture_options capture; std::vector<std::shared_ptr<daemon_target>> targets;
};
class daemon {
 public:
  daemon() noexcept; ~daemon();
  daemon(daemon&&) noexcept; daemon& operator=(daemon&&) noexcept;
  daemon(const daemon&) = delete; daemon& operator=(const daemon&) = delete;
  static runtime::task<daemon> start(std::filesystem::path, daemon_options = {});
  static runtime::task<daemon> attach(std::filesystem::path);
  runtime::task<daemon_status> status();
  runtime::task<void> pause_capture();
  runtime::task<void> resume_capture();
  runtime::task<std::vector<build_plan_action>> build_plan(build_target = {});
  runtime::task<build_handle> build(build_target);
  runtime::task<build_handle> build(action_input);
  runtime::task<void> report_consumer_lag(std::string name, std::uint64_t behind);
  runtime::task<void> shutdown();
  runtime::task<void> wait();
};
```

The opaque handle is either the host returned by `start` or a local client returned by `attach`.
All waiting operations are Capy tasks. A host owns capture, persists the graph on idle or explicit
shutdown, and processes targets in vector order. Cursor publication follows successful target
delivery. During otherwise idle slices, the host validates one pending graph target through the
supplied query registry. `daemon_status` contains capture telemetry, the last invalidated paths,
every target's undelivered drip count and error, graph key, resident-value, byte-use, and ceiling
counts, plus validation backlog, the most recent divergence, and flow-control lag and buckets. Named
in-process consumers report backlog through `report_consumer_lag` without becoming delivery
targets. The local
wire payload is canonical CBOR in the sync protocol's five-byte frame. Build sessions accept the
`build_plan`, `build_repository`, and direct-action `build_start` operations; `build_next` forwards
every public event through that same frame.
Windows entry points return
`errc::unsupported` while Corosio lacks named-pipe transport.

`capture_status` makes overflow and recrawl visible because watcher events are hints, not a
complete account of workspace state. The `drip_filter` insertion point between coalescing and
journal append is intentionally a no-op until liquid #50 supplies inline secret scanning.

TAAP's final object-subject syntax waits on the protocol change (liquid-bridge #1). Build
provenance uses the documented interim digest subject in [provenance.md](provenance.md), and the
`actor` type remains ready for a protocol-defined form beside it.

liquid's readings of the TAAP/1 draft where it is silent (each also filed as protocol feedback on
liquid-bridge #8): the signed bytes are the artifact body **without** its `signatures` field, domain-separated by
the artifact type (§6.6); the signature object's own fields are not signed, and `signature_purpose` must equal
the artifact type or verification fails; identity documents are self-signed as `artifact_type =
"identity_document"` in canonical CBOR with a `signatures` array of §6.7 objects; `previous_document_hash_base64`
is SHA-512/256 over the canonical bytes of the previous document as stored, signatures included. Delegation
chains (§22.5) are not used by drips: a drip is signed by its actor directly. Signing is on by default; a
repository whose `config.signing` is `optional` accepts unsigned drips and marks them, and `status` says so;
verification distinguishes *unsigned* from *invalid*.
For §5.5 and §18.7, `compromised_keys` is an optional signed identity-document array of
`{ key_fingerprint_base32, declared_at, reason? }`; its fingerprint is SHA-512/256 of the
canonical public-key object, encoded as unpadded uppercase base32, and `declared_at` is RFC 3339
UTC. A successor naming its predecessor key there may carry only the new-key signature. Signatures
from that key at or after `declared_at` fail; earlier signatures remain valid and
`verified_identity` reports compromised history. `signer::rotate` persists the chain under
`identity/<identity-fingerprint>/documents/NNN.cbor`, replaces the active local private key after
the successor is signed, and `verify_chain` returns the current key set. Initial and successor
documents share the `identity_document` type: `encode_identity_document` emits their complete
canonical CBOR, `decode_identity_document` rejects malformed or non-canonical input, and
`decode_signature` parses a canonical TAAP/1 signature object. Decoding is separate from
`verify_document` and `verify_chain` so callers explicitly choose their trust policy.
`passphrase_provider`, `key_file_provider`, and `keyring` are implemented for encrypted store
objects. The keyring retains held data-key versions, rotates future writes, rewraps every held
version when the KEK changes, and records revocation after deleting a wrapped version. Recipient,
KMS, Vault, TPM, and Shamir custody remain liquid #51 work; signing remains liquid #4 work.

## Deliberately internal

`src/store/` (chunker, packs, liquid's fixed-width index, GC), `src/frontend/` (module-handle and
frontend-query state), `src/crdt/` (checked automerge-cpp
access, compact/shallow metadata checkpoints, upstream sync framing, and patch observation),
`src/sync/` (canonical metadata and pack wire codecs plus the Corosio/Boost.HTTP client),
`src/capture/` (the journal, OS watchers, coalescing), `src/daemon/` (cursor fan-out), `src/search/`
(manifest, segments, BM25F term records, and positional trigrams), `src/vfs/` (Cloud
Files, File Provider, FUSE), `src/runtime/` (Corosio), `src/identity/` (the TAAP implementation and
key custody providers). Their headers live beside their sources, never under `include/`.
The `src/resolve/` concretizer and CDCL clauses are also private; public callers provide package
records and receive content-addressed concretisations without depending on the engine encoding.

## ABI policy

- **Boost.Capy is a public dependency** (`Boost::capy` linked `PUBLIC`); Corosio,
  `oramasearch/oramacore-cpp` at `9b74ce177f47d63024532ae6000c2cd62c73e0cd`, and the
  `oramasearch/http` client module at `af0353a5b2868d5b8db7fb01ffb5c464f9deb8a4` are private.
  Consumers get Capy transitively and may await liquid tasks from their own Capy coroutines.
- **Static by default.** One binary to ship; liquify and lake link `liquid::liquid` as a pinned
  submodule and take API changes as deliberate pin bumps. No compatibility promise exists between
  pins before 1.0, and none is implied by `SOVERSION 0`.
- **Shared builds are for packaging, not for stability.** Rules 2 and 3 above are what make a
  future stable ABI possible; until 1.0 they are discipline, not a promise.
- **Frontend C ABI v1 exists before 1.0.** `liquid/frontend_abi.h` is separately versioned and does
  not expose the C++ ABI. Language modules use it now; format codecs and package backends may adopt
  dedicated C descriptors later without being loaded into libliquid's core.
- **At 1.0:** semantic versioning of the C++ API, `SOVERSION` bumped on breaks, still no symbol
  versioning. The C++ surface remains distinct from separately versioned module descriptors.

## How to verify

`nm -gU build-shared/libliquid.dylib | grep liquid` lists exactly the symbols of the headers on
this map. Every public function has a test that exercises either its implemented result or its
`result` error path. The `liquid-header-tests` target compiles every public header in its own
translation unit, including the frontend ABI as C11, and a clean installed consumer verifies
`find_package(liquid CONFIG)`. The frontend tests load the Clang module from its shared-object path
and register the same descriptor statically.

## How to change

A new public header is a row in the table above and a paragraph here, in the same PR as the
header. Removing or reordering a field of a value type is a 1.0-and-later conversation, not a
refactor.

## Open

- ~~The async surface.~~ **Resolved (Steve, 2026-09-03):** coroutine-native on Capy from the
  first header; see rules 4 and 6.
- ~~Where the automerge document boundary sits for very large trees.~~ **Resolved
  (2026-09-04):** one document per directory; see [`object-model.md`](object-model.md).
