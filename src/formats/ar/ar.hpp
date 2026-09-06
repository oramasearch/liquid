#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::ar {

struct member {
    std::string name;
    std::vector<std::byte> bytes;
};

[[nodiscard]] auto read(std::span<const std::byte> input) -> result<std::vector<member>>;

} // namespace liquid::formats::ar
