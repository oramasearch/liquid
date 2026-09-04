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
| `liquid/version.hpp` | now | implemented | `version`, `current_version()`, `version_string()` |
| `liquid/export.hpp` | now | implemented | generated; `LIQUID_EXPORT` |
| `liquid/runtime.hpp` | #97 | implemented | the Capy boundary: `task<T>`, `io_result`, `when_all`, `timeout`, `delay`, `as_io`, `block_on`, `spawn`, `offload`, `context` |
| `liquid/error.hpp` | #39 | implemented | `errc`, `error`, `result<T>` |
| `liquid/hash.hpp` | #39, #7 | implemented | `algorithm`, `digest`, `hash(span) -> digest`, keyed-id text round trip |
| `liquid/store.hpp` | #1, #7, #8, #46, #53, #116 | implemented | `store`: memory and streaming-file content in, memory and streaming-file content out; encrypted trust domains and key-version lookup; leases, collection, and pack compaction |
| `liquid/object.hpp` | #2, #50 | implemented | node kinds and canonical record values: `blob`, `tree`, `change`, `scan_result/1` attestation, repository references, conflicts, and reserved symbols |
| `liquid/repository.hpp` | #2, #40, #41, #42, #50, #52, #53, #116 | implemented | `repository`: open, encrypted and plaintext reads, memory and streaming-file writes, redaction, structural key diff, recorded moves/removals, cuts, refs, status, history, readability, nest/split/combine, conflicts, undo, locks |
| `liquid/drip.hpp` | #4, #5, #48, #50 | implemented (#5, #50); declared (#4, #48) | `drip`, `capture`: the scanned journal as a subscription, safe block events, pause and resume |
| `liquid/identity.hpp` | #4, #7, #51, #53, #111 | implemented (#4, #7, #53, #111); declared (#51) | TAAP `actor`, `signer`, identity rotation and chain verification, versioned data-key rotation, keyring and custody providers |
| `liquid/vfs.hpp` | #6 | implemented | `vfs::mount`: FUSE on Linux and optional macFUSE on macOS; `vfs::materialise`: full checkout on every platform |
| `liquid/remote.hpp` | Phase 6 | declared | `remote`: push and pull; declared with the protocol, implemented with lake |

### `liquid/error.hpp`

```cpp
enum class errc : std::uint16_t {
  ok = 0, not_found, already_exists, conflict, invalid_argument, unsupported,
  corrupt, io, locked, unauthorized, unavailable, busy, internal
};
struct error { errc code; std::string message; std::string context; };  // context: a path, a digest, an op id
template <class T> using result = std::expected<T, error>;
std::string_view to_string(errc);
```

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
template <class F> task<std::invoke_result_t<F>> offload(F blocking); // blocking CPU work on the compute pool; resumes on the caller's executor
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
| `offload` | blocking CPU work | the compute pool; resumes on the caller executor |

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

### `liquid/store.hpp`

```cpp
struct store_options {
  algorithm hash_algorithm{algorithm::blake3};
  std::uint64_t pack_size{};
  std::shared_ptr<keyring> keys;       // null preserves plaintext-store behaviour
  std::string write_key;               // empty selects keyring::default_key_id()
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
  std::uint64_t packs_compacted, bytes_reclaimed_by_compaction, bytes_written_by_compaction;
};
class lease { public: runtime::task<void> renew(std::chrono::seconds ttl);
                       runtime::task<void> release(); };
class store {                          // the chunk store: chunks, packs, liquid's own index
 public:
  static runtime::task<store> open(std::filesystem::path, store_options = {});
  runtime::task<digest> put(std::vector<std::byte>);              // chunked, deduplicated, indexed
  runtime::task<digest> put_file(std::filesystem::path);          // streams from disk; a 40 GB file never sits in memory
  runtime::task<void> get_to_file(digest, std::filesystem::path); // streams to disk
  runtime::task<std::vector<std::byte>> get(digest);              // verified on read
  runtime::task<std::uint64_t> read(digest, std::uint64_t offset, std::span<std::byte> into);
  runtime::task<bool> contains(digest);
  runtime::task<std::optional<object_key_version>> key_version(digest);
  runtime::task<lease> lease_root(digest, std::chrono::seconds ttl);   // GC roots are leases
  runtime::task<collection_report> collect(collection_options = {});   // epoch marking
};
```

Chunking (FastCDC), packs, compression, the index and its tuning are internal; none of them
appears in a signature. File I/O is Corosio's `random_access_file` and `stream_file` on the
io_context's worker pool, behind the private boundary. `store_options` names the plaintext hash
algorithm, pack size, optional keyring, optional write key id, and optional durable pack-write
acknowledgement. A non-null keyring enables trust-domain encrypted objects; see
[encryption.md](encryption.md).

The records' shapes and the metadata document are designed in [`object-model.md`](object-model.md).
Lease destruction has no storage effect; expiry is the authority. Collection reports are plain
values, and [gc.md](gc.md) defines their epoch and crash semantics.

### `liquid/object.hpp`

```cpp
enum class node_kind : std::uint8_t { blob, tree, change, action, result_, attestation, repo };
struct blob   { std::vector<digest> chunks; std::uint64_t size;
                std::vector<std::uint64_t> chunk_sizes; };
struct tree_entry { std::string name; std::uint32_t mode; node_kind kind; digest id; };
struct tree   { std::vector<tree_entry> entries; };                   // sorted by name
struct change { change_id id; std::vector<change_id> parents; actor author;
                std::string message; std::chrono::sys_seconds at; digest root; };
struct change_id { digest value; };    // stable across rebase and transplant (features/02)
struct scan_allowance { std::string path, detector; std::uint64_t line; std::string reason; };
struct attestation { digest subject; std::string profile; std::vector<std::string> detectors;
                     std::uint64_t findings_count; std::chrono::sys_seconds at; actor issuer;
                     std::vector<scan_allowance> allowances; std::vector<std::byte> signature; };
result<bool> verify(const attestation&);
```

`chunk_sizes` is appended to the blob value and canonical record so a range reader can locate the
covering logical chunks without reading every preceding chunk. The decoder accepts the earlier
single-chunk record and derives its one size, preserving repositories written before liquid #6.

`attestation` has its first concrete record and `scan_result/1` profile in Phase 1. `action` and
`result_` remain reserved for Phase 2; `repo` is implemented in Phase 1. Two fields are reserved
from the first commit because they
cannot be added later: an optional `symbols` reference on built artifacts (liquid #47) and an
optional `conflict` record on a change (liquid #40).

### `liquid/repository.hpp`

```cpp
struct key_readability { std::string key_id; std::uint64_t version; bool readable; };
struct revision_readability { change_id revision; std::vector<key_readability> keys; };
struct readability_report { std::vector<revision_readability> revisions;
                            std::vector<std::string> limits; };
class repository {
 public:
  static runtime::task<repository> create(std::filesystem::path, repository_options = {});
  static runtime::task<repository> open(std::filesystem::path, repository_options = {});
  runtime::task<tree> root();                            // the working copy is a change
  runtime::task<change_id> head();
  runtime::task<std::vector<std::byte>> read(std::string path);
  runtime::task<std::vector<std::byte>> read_decrypted(std::string path);  // explicit leaf/file decryption
  runtime::task<void> write(std::string path, std::vector<std::byte>);   // becomes a drip
  runtime::task<void> redact(std::string path, std::optional<change_id> = {});
  runtime::task<void> redact(std::string path, std::vector<std::byte> replacement,
                             std::optional<change_id> = {});
  runtime::task<std::vector<std::string>> diff_keys(std::string path, change_id a, change_id b);
  runtime::task<void> write_file(std::string path, std::filesystem::path source);   // streams a large file in; one drip
  runtime::task<void> move(std::string from, std::string to);           // recorded, never inferred; same node_id
  runtime::task<void> remove(std::string path);
  runtime::task<change_id> cut(std::string message);      // name the current change; the working copy starts a new one
  runtime::task<std::vector<ref>> refs();                 // name → change_id
  runtime::task<void> set_ref(std::string name, change_id);
  result<repository_status> status();                     // pending drips, paused, conflicts, oversized directories
  runtime::task<std::vector<change>> log(log_options = {});
  runtime::task<snapshot> at(change_id);                 // time travel
  runtime::task<readability_report> readability(std::optional<change_id> = {});
  runtime::task<void> nest(repository source, std::string path);
  runtime::task<repository> split(std::string path, std::filesystem::path into);
  runtime::task<void> combine(repository other, std::optional<std::string> at);
  runtime::task<std::vector<conflict>> conflicts();      // #40: conflicts are values
  runtime::task<std::vector<operation_record>> operations(log_options = {}); // newest first
  runtime::task<void> undo(op_id);                       // #41: the operation log
  runtime::task<void> lock(std::string path);            // #42
  runtime::task<void> unlock(std::string path);
  // Import primitives (liquid #43; lake's importers use the same calls)
  runtime::task<change_id> record(change_input);                 // a change with explicit parents, author, time, message, metadata, ref
  runtime::task<void> set_metadata(std::string key, std::vector<std::byte>);   // repository-document metadata map
  runtime::task<std::optional<std::vector<std::byte>>> metadata(std::string key);
  runtime::task<std::vector<std::string>> metadata_keys(std::string prefix);
  runtime::task<tree> tree(digest);                              // a tree record from the store (historical snapshots)
  runtime::task<std::vector<std::byte>> blob(digest);            // a blob's bytes by digest
  runtime::task<void> write(std::string path, std::vector<std::byte>, write_options);   // mode-aware
  runtime::task<void> nest_ref(std::string path, repo_ref);      // a nested-repository placeholder without the repository present
};
struct change_input { std::vector<change_ref> parents; actor author; std::chrono::sys_seconds at; std::string message;
                      std::map<std::string, std::string> metadata; std::optional<std::string> ref; };
struct write_options { std::uint32_t mode{0100644}; };
// log_options gains: std::optional<change_id> from; bool first_parent{false};  (full DAG, topological order, by default)
```

`repository_options::encrypt.paths` is an ordered vector of `{ path, key_id, mode,
encrypted_regex }`. The first repository-relative glob match selects leaf or file mode. Leaf mode
is implemented for JSON and `.env`; [leaf-encryption.md](leaf-encryption.md) defines its envelope,
normalization, enforcement, and JSON Pointer key paths.

`ref` is `{ std::string name; change_id id; }`; `repository_status` is a plain record: `pending_drips`, `paused`,
`conflicts`, `oversized_directories` (paths with more than 10,000 entries), `head`.
`repository_options` is a plain record with `checkpoint_interval`, `store`, and `encrypt`; its `store_options`
member selects the object-store hash algorithm and, when configured, its key custody. Pass the
same options to `create` and `open` so later writes and reads use the intended store settings.
Repository change IDs are computed with that selected store algorithm, including IDs returned by
`log` and accepted by `at`.

`key_readability` is `{ key_id, version, readable }`. `revision_readability` is a `change_id` and
the sorted key-version set used by its blobs. `readability_report` contains those revision rows and
the three required rotation, revocation, and metadata-disclosure statements.

`operation_record` is `{ op_id id, std::string kind, std::string from, std::string to, actor by,
std::chrono::sys_seconds at }`. Each operation ID is unique, so it selects one operation for
`undo`. `operations()` returns the repository document's recorded operations newest first.
Existing repository documents without operation metadata remain readable; their records return an
empty actor and epoch timestamp.

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

### `liquid/drip.hpp` and `liquid/identity.hpp`

```cpp
struct drip { drip_id id; change_id base; std::vector<path_op> ops; actor by;
              std::vector<std::byte> signature; std::chrono::sys_seconds at;
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

`capture_status` makes overflow and recrawl visible because watcher events are hints, not a
complete account of workspace state. The `drip_filter` insertion point between coalescing and
journal append is intentionally a no-op until liquid #50 supplies inline secret scanning.

Object subjects (attestations *about* a drip or artifact) wait on the TAAP spec change
(liquid-bridge #1); the `actor` type is written so an object-subject form can be added beside it.

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
the successor is signed, and `verify_chain` returns the current key set.
`passphrase_provider`, `key_file_provider`, and `keyring` are implemented for encrypted store
objects. The keyring retains held data-key versions, rotates future writes, rewraps every held
version when the KEK changes, and records revocation after deleting a wrapped version. Recipient,
KMS, Vault, TPM, and Shamir custody remain liquid #51 work; signing remains liquid #4 work.

## Deliberately internal

`src/store/` (chunker, packs, liquid's fixed-width index, GC), `src/crdt/` (automerge-cpp and the
metadata documents), `src/capture/` (the journal, OS watchers, coalescing), `src/vfs/` (Cloud
Files, File Provider, FUSE), `src/runtime/` (Corosio), `src/identity/` (the TAAP implementation and
key custody providers). Their headers live beside their sources, never under `include/`.

## ABI policy

- **Boost.Capy is a public dependency** (`Boost::capy` linked `PUBLIC`); Corosio is private.
  Consumers get Capy transitively and may await liquid tasks from their own Capy coroutines.
- **Static by default.** One binary to ship; liquify and lake link `liquid::liquid` as a pinned
  submodule and take API changes as deliberate pin bumps. No compatibility promise exists between
  pins before 1.0, and none is implied by `SOVERSION 0`.
- **Shared builds are for packaging, not for stability.** Rules 2 and 3 above are what make a
  future stable ABI possible; until 1.0 they are discipline, not a promise.
- **At 1.0:** semantic versioning of the C++ API, `SOVERSION` bumped on breaks, still no symbol
  versioning. The stable *non-C++* boundary for codecs, language frontends and package backends
  (features/17) is a separate C ABI under `include/liquid/c/`, designed with the first plugin
  (Phase 5), not now.

## How to verify

`nm -gU build-shared/libliquid.dylib | grep liquid` lists exactly the symbols of the headers on
this map. Every public function has a test that exercises either its implemented result or its
`result` error path. The `liquid-header-tests` target compiles every public header in its own
translation unit, and a clean installed consumer verifies `find_package(liquid CONFIG)`.

## How to change

A new public header is a row in the table above and a paragraph here, in the same PR as the
header. Removing or reordering a field of a value type is a 1.0-and-later conversation, not a
refactor.

## Open

- ~~The async surface.~~ **Resolved (Steve, 2026-09-03):** coroutine-native on Capy from the
  first header; see rules 4 and 6.
- **Where the automerge document boundary sits** for very large trees (features/17).
