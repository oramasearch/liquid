// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "mapped_file.hpp"
#include "pack.hpp"

namespace liquid {
class store_backend;
}

namespace liquid::store_detail {

struct index_record {
    std::array<std::byte, 32> bytes{};
    std::uint32_t pack_id{};
    std::uint64_t offset{};
    std::uint32_t length{};
};
static_assert(sizeof(index_record::bytes) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
                  sizeof(std::uint32_t) ==
              48);

struct index_entry {
    digest id{};
    chunk_location location{};
};

struct chunk_index_options {
    std::size_t memtable_entries{1'048'576};
    std::size_t compaction_runs{4};
};

class chunk_index {
public:
    explicit chunk_index(std::filesystem::path root, chunk_index_options options = {});
    chunk_index(std::shared_ptr<::liquid::store_backend> backend,
                std::string prefix,
                std::filesystem::path context,
                chunk_index_options options = {});

    [[nodiscard]] runtime::task<void> open();
    [[nodiscard]] result<void> insert(index_entry entry);
    [[nodiscard]] bool needs_flush(algorithm algorithm_value) const noexcept;
    [[nodiscard]] runtime::task<void> flush();
    [[nodiscard]] result<std::optional<chunk_location>> find(digest id) const;
    [[nodiscard]] runtime::task<void> rebuild(std::vector<index_entry> entries);
    [[nodiscard]] runtime::task<void> close();

    [[nodiscard]] std::uint32_t highest_pack_id() const noexcept {
        return highest_pack_id_;
    }
    void set_highest_pack_id(std::uint32_t value) noexcept {
        highest_pack_id_ = value;
    }
    [[nodiscard]] std::uint64_t collection_epoch() const noexcept {
        return collection_epoch_;
    }
    void set_collection_epoch(std::uint64_t value) noexcept {
        collection_epoch_ = value;
    }

private:
    struct digest_hash {
        std::size_t operator()(const std::array<std::byte, 32>& value) const noexcept;
    };
    struct run {
        std::uint64_t sequence{};
        algorithm algorithm_value{};
        std::string key;
        std::uint64_t record_count{};
        std::uint64_t records_offset{};
        std::span<const std::uint32_t> fanout;
        std::span<const std::byte> bloom;
        mapped_file mapping;
    };
    struct algorithm_state {
        std::unordered_map<std::array<std::byte, 32>, chunk_location, digest_hash> memtable;
        std::vector<run> runs;
    };

    std::filesystem::path root_;
    std::shared_ptr<::liquid::store_backend> backend_;
    std::string prefix_;
    std::map<algorithm, algorithm_state> states_;
    std::uint64_t next_sequence_{1};
    std::uint32_t highest_pack_id_{};
    std::uint64_t collection_epoch_{};
    chunk_index_options options_;

    [[nodiscard]] runtime::task<run>
    map_run(std::uint64_t sequence, algorithm algorithm_value, std::uint64_t expected_count) const;
    [[nodiscard]] std::string key(std::string_view name) const;
    [[nodiscard]] runtime::task<void> flush(algorithm algorithm_value);
    [[nodiscard]] runtime::task<void> compact(algorithm algorithm_value);
    [[nodiscard]] runtime::task<void> write_manifest();
};

} // namespace liquid::store_detail
