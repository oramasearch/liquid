#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace liquid::store_detail {

struct chunk_range {
    std::size_t offset{};
    std::size_t size{};
};

// The constants are part of the on-disk compatibility contract.
inline constexpr std::size_t chunk_minimum = 8U * 1024U;
inline constexpr std::size_t chunk_target = 64U * 1024U;
inline constexpr std::size_t chunk_maximum = 128U * 1024U;

std::vector<chunk_range> fastcdc(std::span<const std::byte> input);

}  // namespace liquid::store_detail
