#pragma once

#include <algorithm>
#include <string_view>

namespace liquid::git_detail {

inline auto tree_entry_less(std::string_view left,
                            bool left_is_directory,
                            std::string_view right,
                            bool right_is_directory) -> bool {
    const auto left_end = left.size() + static_cast<std::size_t>(left_is_directory);
    const auto right_end = right.size() + static_cast<std::size_t>(right_is_directory);
    const auto left_at = [&](std::size_t index) {
        return index == left.size() ? '/' : left[index];
    };
    const auto right_at = [&](std::size_t index) {
        return index == right.size() ? '/' : right[index];
    };
    const auto common = std::min(left_end, right_end);
    for (std::size_t index{}; index < common; ++index) {
        if (left_at(index) != right_at(index))
            return left_at(index) < right_at(index);
    }
    return left_end < right_end;
}

} // namespace liquid::git_detail
