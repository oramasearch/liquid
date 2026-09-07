#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/frontend.hpp"
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

enum class structural_merge_outcome : std::uint8_t { clean, conflict, suspicious };

struct structural_merge_result {
    structural_merge_outcome outcome{structural_merge_outcome::clean};
    std::vector<std::byte> proposal;
    std::vector<std::string> reasons;
    std::optional<std::string> node;
    std::string algorithm{"structural-three-way-v1"};
    std::string grammar;
};

[[nodiscard]] auto merge_structural(const parse_tree& base_tree,
                                    const parse_tree& left_tree,
                                    const parse_tree& right_tree,
                                    std::span<const std::byte> base_source,
                                    std::span<const std::byte> left_source,
                                    std::span<const std::byte> right_source,
                                    const symbol_record* base_symbols = nullptr,
                                    const symbol_record* left_symbols = nullptr,
                                    const symbol_record* right_symbols = nullptr)
    -> result<structural_merge_result>;
[[nodiscard]] auto has_recovery_node(const parse_node& node) -> bool;

} // namespace liquid::repository_detail
