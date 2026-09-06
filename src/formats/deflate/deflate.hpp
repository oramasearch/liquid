#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::deflate {

[[nodiscard]] auto gzip(std::span<const std::byte> input) -> std::vector<std::byte>;
[[nodiscard]] auto gunzip(std::span<const std::byte> input,
                          std::uint64_t maximum_output = 512ULL * 1024ULL * 1024ULL)
    -> result<std::pair<std::vector<std::byte>, std::size_t>>;

} // namespace liquid::formats::deflate
