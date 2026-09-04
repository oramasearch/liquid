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
| `liquid/store.hpp` | #1, #7, #8 | implemented (#1, #7); declared (#8) | `store`: content in, digests out; encrypted trust domains; leases and collection |
| `liquid/object.hpp` | #2 | implemented | node kinds and canonical record values: `blob`, `tree`, `change`, repository references, conflicts, and reserved symbols |
| `liquid/repository.hpp` | #2, #40, #41, #42 | implemented | `repository`: open, read, write, recorded moves/removals, cuts, refs, status, history, nest/split/combine, conflicts, undo, locks |
| `liquid/drip.hpp` | #4, #5, #48 | declared; internal journal implemented | `drip`, `capture`: the journal as a subscription, pause and resume |
| `liquid/identity.hpp` | #4, #7, #51 | implemented (#4, #7); declared (#51) | TAAP `actor`, `signer`, `verify`, keyring and custody providers |
| `liquid/vfs.hpp` | #6 | declared | `mount`: the virtual filesystem over a repository |
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
template <class T> result<T> block_on(task<T>);                // run to completion on a private context (CLI, tests)
void spawn(task<void>, std::function<void(result<void>)> on_done);   // fire-and-forget on the shared pool
template <class F> task<std::invoke_result_t<F>> offload(F blocking); // blocking work on the pool; resumes on the caller's executor
}
```

Corosio never appears here; `src/runtime/net.hpp` is the private adapter TUs include for
sockets, timers and files. Coroutine functions take their parameters **by value** and are never
capturing lambdas (the frame outlives the call). A task is lazy and `[[nodiscard]]`; after
`offload` the continuation is back on the caller's executor before any Corosio object is touched.

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
class store {                          // the chunk store: chunks, packs, liquid's own index
 public:
  static runtime::task<store> open(std::filesystem::path, store_options = {});
  runtime::task<digest> put(std::vector<std::byte>);              // chunked, deduplicated, indexed
  runtime::task<std::vector<std::byte>> get(digest);              // verified on read
  runtime::task<std::uint64_t> read(digest, std::uint64_t offset, std::span<std::byte> into);
  runtime::task<bool> contains(digest);
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

### `liquid/object.hpp`

```cpp
enum class node_kind : std::uint8_t { blob, tree, change, action, result_, attestation, repo };
struct blob   { std::vector<digest> chunks; std::uint64_t size; };
struct tree_entry { std::string name; std::uint32_t mode; node_kind kind; digest id; };
struct tree   { std::vector<tree_entry> entries; };                   // sorted by name
struct change { change_id id; std::vector<change_id> parents; actor author;
                std::string message; std::chrono::sys_seconds at; digest root; };
struct change_id { digest value; };    // stable across rebase and transplant (features/02)
```

`action`, `result_`, `attestation` and `repo` are declared as kinds now and given their records in
Phases 2, 2, 2 and 1 respectively. Two fields are reserved from the first commit because they
cannot be added later: an optional `symbols` reference on built artifacts (liquid #47) and an
optional `conflict` record on a change (liquid #40).

### `liquid/repository.hpp`

```cpp
class repository {
 public:
  static runtime::task<repository> create(std::filesystem::path, repository_options = {});
  static runtime::task<repository> open(std::filesystem::path);
  runtime::task<tree> root();                            // the working copy is a change
  runtime::task<change_id> head();
  runtime::task<std::vector<std::byte>> read(std::string path);
  runtime::task<void> write(std::string path, std::vector<std::byte>);   // becomes a drip
  runtime::task<void> move(std::string from, std::string to);           // recorded, never inferred; same node_id
  runtime::task<void> remove(std::string path);
  runtime::task<change_id> cut(std::string message);      // name the current change; the working copy starts a new one
  runtime::task<std::vector<ref>> refs();                 // name → change_id
  runtime::task<void> set_ref(std::string name, change_id);
  result<repository_status> status();                     // pending drips, paused, conflicts, oversized directories
  runtime::task<std::vector<change>> log(log_options = {});
  runtime::task<snapshot> at(change_id);                 // time travel
  runtime::task<void> nest(repository source, std::string path);
  runtime::task<repository> split(std::string path, std::filesystem::path into);
  runtime::task<void> combine(repository other, std::optional<std::string> at);
  runtime::task<std::vector<conflict>> conflicts();      // #40: conflicts are values
  runtime::task<void> undo(op_id);                       // #41: the operation log
  runtime::task<void> lock(std::string path);            // #42
  runtime::task<void> unlock(std::string path);
};
```

`ref` is `{ std::string name; change_id id; }`; `repository_status` is a plain record: `pending_drips`, `paused`,
`conflicts`, `oversized_directories` (paths with more than 10,000 entries), `head`.

### `liquid/drip.hpp` and `liquid/identity.hpp`

```cpp
struct drip { drip_id id; change_id base; std::vector<path_op> ops; actor by;
              std::vector<std::byte> signature; std::chrono::sys_seconds at; };
struct capture_options { std::vector<std::string> exclude;            // glob patterns from config
                         std::chrono::milliseconds coalesce_window{200};
                         bool attribution{false}; };                   // Phase 2 turns it on
class capture {                        // the journal, as a subscription
 public:
  static runtime::task<capture> open(std::filesystem::path workspace, capture_options = {});   // starts the watcher
  runtime::task<void> close();
  result<subscription> subscribe(std::function<void(const drip&)>);   // registration is synchronous
  runtime::task<void> pause();  runtime::task<void> resume();  result<capture_status> status();
};
struct actor { std::string taap_id; };                   // TAAP/1 subject
class signer { public: runtime::task<signature> sign(std::string artifact_type, std::vector<std::byte> canonical_body); };  // TAAP/1 §6.6; custody may be remote
result<bool> verify(const actor&, std::string_view artifact_type, std::span<const std::byte> canonical_body, const signature&);
struct signature { std::string algorithm; std::string signing_key_identifier; std::string signing_key_label;
                   std::string signature_purpose; std::string signed_at; std::vector<std::byte> bytes; };   // TAAP/1 §6.7
struct argon2_parameters { std::uint32_t memory_kib; std::uint32_t iterations; std::uint32_t lanes; };
class key_provider { protected: virtual result<std::array<std::byte, 32>> derive_kek(span, argon2_parameters) const = 0; };
class passphrase_provider final : public key_provider { public: result<std::array<std::byte, 32>> derive(span, argon2_parameters = {}) const; };
class key_file_provider final : public key_provider { public: static runtime::task<std::shared_ptr<key_file_provider>> open(std::filesystem::path); };
class keyring { public: result<void> add(std::string, std::shared_ptr<key_provider>); result<std::string> default_key_id() const; result<void> set_default_key_id(std::string); runtime::task<void> rewrap(std::string, std::shared_ptr<key_provider>); };
```

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
`passphrase_provider`, `key_file_provider`, and `keyring` are implemented for encrypted store
objects. Recipient, KMS, Vault, TPM, and Shamir custody remain liquid #51 work; signing remains
liquid #4 work.

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
