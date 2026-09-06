#pragma once

#include <optional>
#include <span>
#include <string>

#include "liquid/object.hpp"

namespace liquid::repository_detail {

[[nodiscard]] auto entry_conflict(std::string path,
                                  const digest& left,
                                  const digest& right,
                                  node_id left_node,
                                  node_id right_node) -> std::optional<conflict>;
[[nodiscard]] auto three_way_text_conflict(std::string path,
                                           const digest& base,
                                           const digest& left,
                                           const digest& right,
                                           std::span<const std::byte> left_bytes,
                                           std::span<const std::byte> right_bytes)
    -> std::optional<conflict>;

} // namespace liquid::repository_detail
