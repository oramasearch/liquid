#pragma once

#include <span>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

LIQUID_EXPORT auto compare_structural(const parse_tree& old_tree,
                                      const parse_tree& new_tree,
                                      std::span<const std::byte> old_source,
                                      std::span<const std::byte> new_source)
    -> result<structural_diff>;

} // namespace liquid::repository_detail
