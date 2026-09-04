#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "pack.hpp"

namespace liquid::store_detail {

// This is serialized explicitly, rather than written as a C++ struct: its
// portable record representation is exactly 48 bytes.
struct index_record {
    std::array<std::byte, 32> bytes{};
    std::uint32_t pack_id{};
    std::uint64_t offset{};
    std::uint32_t length{};
};
static_assert(sizeof(index_record::bytes) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
                  sizeof(std::uint32_t) == 48);

struct index_entry {
    digest id{};
    chunk_location location{};
};

class chunk_index {
public:
    explicit chunk_index(std::filesystem::path root);

    [[nodiscard]] runtime::task<void> open();
    [[nodiscard]] runtime::task<void> insert(index_entry entry);
    [[nodiscard]] runtime::task<void> flush();
    [[nodiscard]] runtime::task<std::optional<chunk_location>> find(digest id);
    [[nodiscard]] runtime::task<void> rebuild(std::vector<index_entry> entries);
    [[nodiscard]] runtime::task<void> close();

    [[nodiscard]] std::uint32_t highest_pack_id() const noexcept { return highest_pack_id_; }
    void set_highest_pack_id(std::uint32_t value) noexcept { highest_pack_id_ = value; }

private:
    struct run {
        std::uint64_t sequence{};
        algorithm algorithm_value{};
        std::filesystem::path path;
        std::vector<index_entry> entries;
        std::vector<std::byte> bloom;
    };
    struct algorithm_state {
        std::vector<index_entry> memtable;
        std::vector<run> runs;
    };
    std::filesystem::path root_;
    std::map<algorithm, algorithm_state> states_;
    std::uint64_t next_sequence_{1};
    std::uint32_t highest_pack_id_{};

    [[nodiscard]] runtime::task<void> write_manifest();
    [[nodiscard]] runtime::task<void> compact(algorithm algorithm_value);
};

}  // namespace liquid::store_detail
