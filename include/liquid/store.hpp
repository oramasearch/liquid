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
    [[nodiscard]] virtual runtime::task<void> barrier() = 0;
};

[[nodiscard]] LIQUID_EXPORT std::shared_ptr<store_backend>
make_disk_store_backend(std::filesystem::path root, bool durable = false);

class repository;
namespace repository_detail {
struct store_access;
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
    // other process writes the root.
    bool exclusive_owner{};
    // How long an exclusive open waits for the current owner to release the root. Zero makes one
    // attempt. Only meaningful with exclusive_owner.
    std::chrono::milliseconds owner_wait{};
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
    friend struct repository_detail::store_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
