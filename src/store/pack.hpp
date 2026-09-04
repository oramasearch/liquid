#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/hash.hpp"
#include "store/index.hpp"

namespace liquid::store_detail {

inline constexpr std::size_t pack_record_header_size = 48;

class pack_store {
public:
    static result<pack_store> open(
        const std::filesystem::path& directory, std::uint64_t target_size);

    result<chunk_location> append(const digest& id, std::span<const std::byte> bytes);
    result<std::vector<std::byte>> read(
        const digest& expected, const chunk_location& location) const;

private:
    pack_store(std::filesystem::path directory, std::uint64_t target_size, std::uint32_t pack_id);

    result<void> ensure_table(std::uint32_t pack_id) const;
    std::filesystem::path pack_path(std::uint32_t pack_id) const;
    std::filesystem::path table_path(std::uint32_t pack_id) const;

    std::filesystem::path directory_;
    std::uint64_t target_size_;
    std::uint32_t pack_id_;
};

}  // namespace liquid::store_detail
