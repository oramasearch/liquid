#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/object.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT store_backend_entry {
    std::string key;
    std::uint64_t size{};
};

class LIQUID_EXPORT store_backend {
public:
    virtual ~store_backend() = default;

    [[nodiscard]] virtual runtime::task<void> put(std::string key,
                                                  std::vector<std::byte> bytes) = 0;
    // Conditional create (liquid #326): stores bytes under key only when no object exists there,
    // and fails errc::already_exists otherwise. The test and the write are one atomic decision
    // the backend makes, so of two concurrent creators exactly one succeeds. Readers see the
    // complete object or none, as with put. A repository's exclusive create needs it; the default
    // refuses with errc::unsupported rather than emulate it with a racy exists-then-put.
    [[nodiscard]] virtual runtime::task<void> put_if_absent(std::string key,
                                                            std::vector<std::byte> bytes);
    [[nodiscard]] virtual runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) = 0;
    [[nodiscard]] virtual runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) = 0;
    [[nodiscard]] virtual runtime::task<void> remove(std::string key) = 0;
    [[nodiscard]] virtual runtime::task<std::vector<store_backend_entry>>
    list(std::string prefix) = 0;
    // Ordering and durability (liquid #344). Barriers cut the mutations issued through a backend
    // (put, put_if_absent, put_range, remove) into intervals. When barrier() succeeds, every
    // mutation that succeeded before it is durable, so a crash never keeps a mutation issued after
    // a barrier without everything issued before it. Within one interval a crash may keep any
    // subset of the interval's mutations, with one exception: the mutations of one key keep their
    // issue order, so a crash keeps a prefix of each key's own mutations (a put_range never
    // survives without the ranges before it, and an earlier put never overwrites a later one).
    // Reads see every mutation that succeeded. Liquid puts a barrier between any two mutations of
    // different keys whose order its recovery depends on, and relies on nothing stronger, so a
    // backend may send one interval's writes to different keys concurrently. A backend may also
    // give more: Lake's workspace store keeps a prefix of the whole issue order. See
    // docs/library/store.md, "Ordering between barriers".
    [[nodiscard]] virtual runtime::task<void> barrier() = 0;

    // A view of this backend whose every mutation carries an owner's fence (liquid #327). An open
    // with store_options::owner calls it once with the granted fence and sends every later backend
    // call through the returned view, so the remote can refuse a mutation whose fence is not the
    // current holder's with errc::locked. The view shares this backend's objects. The default
    // returns errc::unsupported: a backend that cannot carry a fence cannot serve an injected
    // owner.
    [[nodiscard]] virtual result<std::shared_ptr<store_backend>> fenced(std::uint64_t fence) const;
};

[[nodiscard]] LIQUID_EXPORT std::shared_ptr<store_backend>
make_disk_store_backend(std::filesystem::path root, bool durable = false);

// One holder's ownership of a store (liquid #327). An owner's acquire returns a grant: the fence is
// an opaque token that rises with every new holder and that the store passes to its backend through
// store_backend::fenced; lost() turns true once the holder may no longer write, either because the
// owner revoked the grant (its lease lapsed or was taken) or because the backend refused a stale
// fence. Copies share one state, so the owner can revoke the grant the store holds.
class LIQUID_EXPORT owner_grant {
public:
    owner_grant() noexcept;
    explicit owner_grant(std::uint64_t fence);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t fence() const noexcept;
    [[nodiscard]] bool lost() const noexcept;
    void revoke() const noexcept;

private:
    struct state;
    std::shared_ptr<state> state_;
};

// The owner lock of one store, injected through store_options::owner (liquid #327). acquire waits
// up to `wait` for the current holder to release and then fails with errc::locked naming it; a zero
// wait makes one attempt. `holder` describes the acquiring process for the refusal another opener
// sees. release gives the store up so a waiting successor can acquire it. One owner object serves
// one open store at a time.
class LIQUID_EXPORT store_owner {
public:
    virtual ~store_owner() = default;

    [[nodiscard]] virtual runtime::task<owner_grant> acquire(std::string holder,
                                                             std::chrono::milliseconds wait) = 0;
    [[nodiscard]] virtual runtime::task<void> release() = 0;
};

// The disk owner: an exclusive flock (LockFileEx on Windows) on `<root>/store.lock`, the lock that
// store_options::exclusive_owner takes on a disk store (liquid #321). Its fence is always zero; the
// kernel lock excludes every other writer for as long as it is held.
[[nodiscard]] LIQUID_EXPORT std::shared_ptr<store_owner>
make_disk_store_owner(std::filesystem::path root);

class repository;
namespace repository_detail {
struct store_access;
}
namespace store_detail {
class store_backend;
}

class LIQUID_EXPORT blob_write_through {
public:
    virtual ~blob_write_through() = default;

    [[nodiscard]] virtual runtime::task<void> put(digest content,
                                                  std::span<const std::byte> bytes,
                                                  std::span<const digest> chunks,
                                                  std::span<const std::uint64_t> chunk_sizes) = 0;
};

using hydration_observer = std::function<runtime::task<void>(
    digest content, digest chunk, std::uint64_t bytes, bool content_complete)>;
using remote_access_observer = std::function<void(digest content)>;

struct LIQUID_EXPORT store_options {
    algorithm hash_algorithm{algorithm::blake3};
    std::uint64_t pack_size{};
    std::shared_ptr<keyring> keys;
    std::map<std::string, std::string> custody;
    std::string write_key;
    bool durable{};
    std::map<std::string, std::shared_ptr<remote_blob_source>> remote_sources;
    std::shared_ptr<blob_write_through> write_through;
    hydration_observer on_hydration;
    remote_access_observer on_remote_access;
    std::shared_ptr<store_backend> backend;
    // Exclusive cross-process ownership of a disk store root (liquid #321). When set, open takes
    // the root's owner lock before it reads or repairs anything, and the store holds it until the
    // handle is destroyed or its process exits. Another exclusive opener then waits up to its own
    // owner_wait and fails with errc::locked naming the holder. A store kept by a long-running
    // service sets it, so a successor process can take over from a draining predecessor. Opens
    // without it take no lock and keep the single-process contract: the caller must ensure no
    // other process writes the root. A backend without a local root needs an injected owner.
    bool exclusive_owner{};
    // How long an exclusive open waits for the current owner to release the root. Zero makes one
    // attempt. Only meaningful with exclusive_owner.
    std::chrono::milliseconds owner_wait{};
    // An injected owner lock (liquid #327). When set, open acquires it before it reads or repairs
    // anything, opens the backend's fenced view with the grant's fence, and holds the grant until
    // close() or destruction. It implies exclusive ownership and works with any backend whose
    // fenced() is supported. Once the grant is lost, every later backend mutation fails with
    // errc::locked and the store must be closed.
    std::shared_ptr<store_owner> owner;
    // Lets reads continue after the injected owner's grant is lost. Without it reads also fail with
    // errc::locked.
    bool read_after_owner_lost{};
};

enum class blob_availability : std::uint8_t { absent, remote, local };

struct LIQUID_EXPORT object_key_version {
    std::string key_id;
    std::uint64_t version{};
    bool readable{};
};

struct LIQUID_EXPORT collection_options {
    bool dry_run{};
    std::chrono::seconds min_lease_age{};
    bool explain{};
    double compact_below{0.5};
    bool compact_only{};
    bool repack{};
};

struct LIQUID_EXPORT retained_chunk {
    digest id{};
    std::string reason;
};

struct LIQUID_EXPORT partial_pack {
    std::uint32_t id{};
    double live_fraction{};
};

struct LIQUID_EXPORT collection_report {
    std::uint64_t roots{};
    std::uint64_t live_objects{};
    std::uint64_t live_chunks{};
    std::uint64_t dead_chunks{};
    std::uint64_t bytes_reclaimed{};
    std::uint64_t bytes_reclaimable_in_partially_dead_packs{};
    std::uint64_t packs_deleted{};
    std::uint64_t epoch{};
    std::vector<std::string> unknown_kinds;
    std::vector<partial_pack> partially_dead_packs;
    std::vector<retained_chunk> retained_chunks;
    std::vector<retained_chunk> retained_objects;
    std::map<std::string, std::uint64_t> retained_by_rule;
    std::uint64_t packs_compacted{};
    std::uint64_t bytes_reclaimed_by_compaction{};
    std::uint64_t bytes_written_by_compaction{};
    std::uint64_t deltas_rebased{};
};

class LIQUID_EXPORT lease {
public:
    lease() noexcept;
    ~lease();
    lease(lease&&) noexcept;
    lease& operator=(lease&&) noexcept;
    lease(const lease&) = delete;
    lease& operator=(const lease&) = delete;

    [[nodiscard]] runtime::task<void> renew(std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<void> release();

private:
    friend class store;
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT store {
public:
    store() noexcept;
    ~store();
    store(store&&) noexcept;
    store& operator=(store&&) noexcept;
    store(const store&) = delete;
    store& operator=(const store&) = delete;

    [[nodiscard]] static runtime::task<store> open(std::filesystem::path path,
                                                   store_options options = {});
    [[nodiscard]] runtime::task<digest> put(std::vector<std::byte> input);
    [[nodiscard]] runtime::task<digest> put_file(std::filesystem::path source);
    [[nodiscard]] runtime::task<void> get_to_file(digest id, std::filesystem::path destination);
    [[nodiscard]] runtime::task<std::vector<std::byte>> get(digest id);
    [[nodiscard]] runtime::task<std::uint64_t>
    read(digest id, std::uint64_t offset, std::span<std::byte> into);
    [[nodiscard]] runtime::task<bool> contains(digest id);
    [[nodiscard]] runtime::task<blob_availability> has(digest id);
    [[nodiscard]] runtime::task<void>
    register_remote_source(std::string id, std::shared_ptr<remote_blob_source> source);
    [[nodiscard]] runtime::task<void> register_remote(blob value);
    [[nodiscard]] runtime::task<void> evict(digest id);
    [[nodiscard]] runtime::task<std::uint64_t> local_bytes();
    [[nodiscard]] std::vector<digest> attestations(digest subject) const;
    [[nodiscard]] runtime::task<std::optional<object_key_version>> key_version(digest id);
    [[nodiscard]] runtime::task<lease> lease_root(digest id, std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options = {});
    // Closes the store and then releases its owner, awaiting the release, so a waiting successor
    // can take over. Finish in-flight operations first. The handle is empty afterwards.
    [[nodiscard]] runtime::task<void> close();

private:
    friend class repository;

    [[nodiscard]] result<std::filesystem::path> root_path() const;
    [[nodiscard]] result<store_options> configuration() const;
    [[nodiscard]] runtime::task<lease>
    lease_root(digest id, std::chrono::seconds ttl, std::string owner);
    [[nodiscard]] runtime::task<digest> put_verified(digest id, std::vector<std::byte> input);
    [[nodiscard]] result<blob> blob_for(digest id) const;
    [[nodiscard]] result<std::uint64_t> remote_bytes(digest blob_record) const;
    [[nodiscard]] runtime::task<void> redact(std::vector<digest> ids);
    [[nodiscard]] std::vector<digest> object_ids(std::string_view kind) const;
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input);
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input,
                                                  std::string delta_scope);
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input,
                                                  algorithm hash_algorithm);
    [[nodiscard]] runtime::task<digest> put_encrypted(std::vector<std::byte> input,
                                                      std::string key_id);
    void begin_object_batch();
    void abort_object_batch();
    [[nodiscard]] runtime::task<void> commit_object_batch();
    // The open store's implementation, for a whole-repository transfer (liquid #328); null when
    // the handle is empty.
    [[nodiscard]] store_detail::store_backend* implementation() const noexcept;
    friend struct repository_detail::store_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
