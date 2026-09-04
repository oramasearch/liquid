#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace liquid::store_detail {

inline constexpr std::size_t chunk_min_size = 8 * 1024;
inline constexpr std::size_t chunk_target_size = 64 * 1024;
inline constexpr std::size_t chunk_max_size = 128 * 1024;

struct chunk_range {
    std::size_t offset;
    std::size_t length;
};

[[nodiscard]] std::vector<chunk_range> chunk(std::span<const std::byte> input);

}  // namespace liquid::store_detail
