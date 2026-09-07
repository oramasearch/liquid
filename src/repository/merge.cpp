#include "repository/merge.hpp"

#include <algorithm>

namespace liquid::repository_detail {

auto entry_conflict(std::string path,
                    const digest& left,
                    const digest& right,
                    node_id left_node,
                    node_id right_node) -> std::optional<conflict> {
    if (left.alg == right.alg && left.bytes == right.bytes) {
        return std::nullopt;
    }
    return conflict{
        .base = left,
        .left = left,
        .right = right,
        .region = {.path = std::move(path), .nodes = conflict_node_range{left_node, right_node}},
        .kind = conflict_kind::structural,
        .explanation = "both repositories contain a different value at this path"};
}

auto three_way_text_conflict(std::string path,
                             const digest& base,
                             const digest& left,
                             const digest& right,
                             std::span<const std::byte> left_bytes,
                             std::span<const std::byte> right_bytes) -> std::optional<conflict> {
    if ((left.alg == right.alg && left.bytes == right.bytes) ||
        (left.alg == base.alg && left.bytes == base.bytes) ||
        (right.alg == base.alg && right.bytes == base.bytes)) {
        return std::nullopt;
    }
    auto prefix = std::size_t{};
    while (prefix < left_bytes.size() && prefix < right_bytes.size() &&
           left_bytes[prefix] == right_bytes[prefix]) {
        ++prefix;
    }
    auto suffix = std::size_t{};
    while (suffix < left_bytes.size() - prefix && suffix < right_bytes.size() - prefix &&
           left_bytes[left_bytes.size() - suffix - 1] ==
               right_bytes[right_bytes.size() - suffix - 1]) {
        ++suffix;
    }
    const auto end = std::max(left_bytes.size(), right_bytes.size()) - suffix;
    return conflict{
        .base = base,
        .left = left,
        .right = right,
        .region = {.path = std::move(path), .bytes = conflict_byte_range{prefix, end - prefix}},
        .kind = conflict_kind::text,
        .explanation = "both sides changed the same text region"};
}

} // namespace liquid::repository_detail
