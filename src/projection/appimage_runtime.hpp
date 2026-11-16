// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "liquid/error.hpp"

namespace liquid::projection_detail {

[[nodiscard]] auto appimage_runtime(std::string_view architecture)
    -> result<std::span<const std::byte>>;

} // namespace liquid::projection_detail
