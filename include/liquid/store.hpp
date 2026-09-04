#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT store_options {
    algorithm hash_algorithm{algorithm::blake3};
    std::uint64_t pack_size{};
    std::shared_ptr<keyring> keys;
    std::string write_key;
    bool durable{};
};

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
    std::uint64_t packs_compacted{};
    std::uint64_t bytes_reclaimed_by_compaction{};
    std::uint64_t bytes_written_by_compaction{};
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
    [[nodiscard]] runtime::task<std::optional<object_key_version>> key_version(digest id);
    [[nodiscard]] runtime::task<lease> lease_root(digest id, std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options = {});

private:
    friend class repository;

    [[nodiscard]] result<blob> blob_for(digest id) const;
    [[nodiscard]] runtime::task<void> redact(std::vector<digest> ids);
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
