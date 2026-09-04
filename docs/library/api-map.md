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
4. **No third-party type is visible.** Not Zetta, ever. Not Boost.Capy, Corosio, automerge-cpp,
   RocksDB, BLAKE3, tree-sitter. They live behind the handles. If a facility from one of them must
   reach a client, it is wrapped in a liquid type.
5. **Every reference carries its algorithm.** A `digest` is `(algorithm, bytes)`; its text form is
   `blake3:<hex>` or `sha256:<hex>`. Nothing in the API takes bare bytes as an identity.
6. **Synchronous for Phase 1.** Every call may block on I/O and says so. The async surface arrives
   with the daemon (Phase 4) as its own header and is a decision then, not a leak now (see
   *Open*).
7. **Exported explicitly.** Every public class and free function carries `LIQUID_EXPORT`. Templates
   and inline functions in public headers touch only public types; nothing inline reaches private
   state.
8. **One umbrella.** `liquid/liquid.hpp` includes every public header; clients may include either.

## The headers

| Header | Phase | What it exposes |
|---|---|---|
| `liquid/liquid.hpp` | now | umbrella |
| `liquid/version.hpp` | now | `version`, `current_version()`, `version_string()` |
| `liquid/export.hpp` | now | generated; `LIQUID_EXPORT` |
| `liquid/error.hpp` | #39 | `errc`, `error`, `result<T>` |
| `liquid/hash.hpp` | #39 | `algorithm`, `digest`, `hash(span) -> digest`, text round trip |
| `liquid/store.hpp` | #1, #8 | `store`: content in, digests out; leases and collection |
| `liquid/object.hpp` | #2 | node kinds and their records; `blob`, `tree`, `change`, and the reserved shapes |
| `liquid/repository.hpp` | #2, #40, #41, #42 | `repository`: open, read, write, history, nest/split/combine, conflicts, undo, locks |
| `liquid/drip.hpp` | #4, #5, #48 | `drip`, `capture`: the journal as a subscription, pause and resume |
| `liquid/identity.hpp` | #4, #51 | TAAP `actor`, `signer`, `verify`, `key_provider` |
| `liquid/vfs.hpp` | #6 | `mount`: the virtual filesystem over a repository |
| `liquid/remote.hpp` | Phase 6 | `remote`: push and pull; declared with the protocol, implemented with lake |

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

### `liquid/hash.hpp`

```cpp
enum class algorithm : std::uint8_t { blake3 = 1, sha256 = 2 };
struct digest { algorithm alg; std::array<std::byte, 32> bytes; };   // fixed width; both are 256-bit
result<digest> hash(algorithm, std::span<const std::byte>);
std::string to_string(const digest&);                 // "blake3:9f86d0..."
result<digest> parse_digest(std::string_view);
```

BLAKE3 is the default everywhere; SHA-256 exists for OCI and REAPI interop. Adding an algorithm
is adding an enumerator; a repository may hold both (liquid #44).

### `liquid/store.hpp`

```cpp
class store {                          // the chunk store: chunks, packs, the index (RocksDB)
 public:
  static result<store> open(const std::filesystem::path&, const store_options& = {});
  result<digest> put(std::span<const std::byte>);                 // chunked, deduplicated, indexed
  result<std::vector<std::byte>> get(const digest&);              // verified on read
  result<std::uint64_t> read(const digest&, std::uint64_t offset, std::span<std::byte> into);
  result<bool> contains(const digest&);
  result<lease> lease_root(const digest&, std::chrono::seconds ttl);   // GC roots are leases
  result<collection_report> collect(const collection_options& = {});   // epoch marking
};
```

Chunking (FastCDC), packs, compression, the index engine and its tuning are internal; none of
them appears in a signature. `store_options` names the algorithm and the pack size only.

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
  static result<repository> create(const std::filesystem::path&, const repository_options& = {});
  static result<repository> open(const std::filesystem::path&);
  result<tree> root();                                   // the working copy is a change
  result<change_id> head();
  result<std::vector<std::byte>> read(std::string_view path);
  result<void> write(std::string_view path, std::span<const std::byte>);   // becomes a drip
  result<std::vector<change>> log(const log_options& = {});
  result<snapshot> at(const change_id&);                 // time travel
  result<void> nest(const repository& source, std::string_view path);
  result<repository> split(std::string_view path, const std::filesystem::path& into);
  result<void> combine(const repository& other, std::optional<std::string_view> at);
  result<std::vector<conflict>> conflicts();             // #40: conflicts are values
  result<void> undo(const op_id&);                       // #41: the operation log
  result<void> lock(std::string_view path);              // #42
  result<void> unlock(std::string_view path);
};
```

### `liquid/drip.hpp` and `liquid/identity.hpp`

```cpp
struct drip { drip_id id; change_id base; std::vector<path_op> ops; actor by;
              std::vector<std::byte> signature; std::chrono::sys_seconds at; };
class capture {                        // the journal, as a subscription
 public:
  result<subscription> subscribe(std::function<void(const drip&)>);
  result<void> pause();  result<void> resume();  result<capture_status> status();
};
struct actor { std::string taap_id; };                   // TAAP/1 subject
class signer { public: result<std::vector<std::byte>> sign(std::span<const std::byte>); };
result<bool> verify(const actor&, std::span<const std::byte> payload, std::span<const std::byte> signature);
class key_provider { /* pluggable custody: file, age, passphrase, later KMS/TPM (liquid #51) */ };
```

Object subjects (attestations *about* a drip or artifact) wait on the TAAP spec change
(liquid-bridge #1); the `actor` type is written so an object-subject form can be added beside it.

## Deliberately internal

`src/store/` (chunker, packs, the RocksDB index, GC), `src/crdt/` (automerge-cpp and the
metadata documents), `src/capture/` (the journal, OS watchers, coalescing), `src/vfs/` (Cloud
Files, File Provider, FUSE), `src/net/` (Corosio), `src/identity/` (the TAAP implementation and
key custody providers). Their headers live beside their sources, never under `include/`.

## ABI policy

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
this map. Every public function has a test that exercises its `result` error path. A clean
consumer (`find_package(liquid CONFIG)`) compiles each header in isolation.

## How to change

A new public header is a row in the table above and a paragraph here, in the same PR as the
header. Removing or reordering a field of a value type is a 1.0-and-later conversation, not a
refactor.

## Open

- **The async surface.** Whether Phase 4's daemon API exposes coroutines (and therefore a Capy
  type), callbacks, or a liquid-owned awaitable. Decide with the daemon (liquid #21).
- **Where the automerge document boundary sits** for very large trees (features/17).
