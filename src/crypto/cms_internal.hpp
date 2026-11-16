// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::cms::detail {

auto signed_content(std::span<const std::byte> encoded) -> result<std::vector<std::byte>>;

} // namespace liquid::cms::detail
