// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

[[nodiscard]] auto text_hunks(std::span<const std::byte> old_bytes,
                              std::span<const std::byte> new_bytes)
    -> std::optional<std::vector<line_change>>;

} // namespace liquid::repository_detail
