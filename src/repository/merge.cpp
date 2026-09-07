#include "repository/merge.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

namespace liquid::repository_detail {
namespace {

constexpr auto newline = std::byte{static_cast<unsigned char>('\n')};

struct unit_view {
    const frontend_unit* value{};
    std::span<const std::byte> source;
};

struct merge_hunk {
    std::uint64_t start{};
    std::uint64_t end{};
    std::vector<std::byte> replacement;
    unsigned side{};
    std::string name;
};

auto equal_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto definitions(const parse_tree& tree, std::span<const std::byte> source)
    -> std::map<std::string, unit_view> {
    auto output = std::map<std::string, unit_view>{};
    auto duplicates = std::set<std::string>{};
    for (const auto& unit : tree.units) {
        if (unit.kind != frontend_unit_kind::definition) {
            continue;
        }
        if (!output.emplace(unit.name, unit_view{&unit, source}).second) {
            duplicates.insert(unit.name);
        }
    }
    for (const auto& name : duplicates) {
        output.erase(name);
    }
    return output;
}

auto unit_bytes(const unit_view& unit) -> std::vector<std::byte> {
    const auto start = unit.value->range.start.byte;
    const auto end = unit.value->range.end.byte;
    if (start > end || end > unit.source.size()) {
        return {};
    }
    return {unit.source.begin() + static_cast<std::ptrdiff_t>(start),
            unit.source.begin() + static_cast<std::ptrdiff_t>(end)};
}

auto changed(const unit_view& base, const unit_view& side) -> bool {
    return !equal_digest(base.value->fingerprint, side.value->fingerprint);
}

auto insertion_offset(const parse_tree& base_tree,
                      const parse_tree& side_tree,
                      const frontend_unit& added,
                      const std::map<std::string, unit_view>& base) -> std::uint64_t {
    const auto found =
        std::ranges::find_if(side_tree.units, [&](const auto& unit) { return &unit == &added; });
    if (found != side_tree.units.end()) {
        for (auto next = std::next(found); next != side_tree.units.end(); ++next) {
            if (const auto anchor = base.find(next->name); anchor != base.end()) {
                return anchor->second.value->range.start.byte;
            }
        }
        for (auto previous = found; previous != side_tree.units.begin();) {
            --previous;
            if (const auto anchor = base.find(previous->name); anchor != base.end()) {
                return anchor->second.value->range.end.byte;
            }
        }
    }
    return base_tree.root.range.end.byte;
}

void add_reason(structural_merge_result& output, std::string reason, std::string node) {
    output.outcome = structural_merge_outcome::suspicious;
    output.reasons.push_back(std::move(reason));
    if (!output.node && !node.empty()) {
        output.node = std::move(node);
    }
}

auto symbol_names(const symbol_record* record, symbol_role role) -> std::set<std::string> {
    auto output = std::set<std::string>{};
    if (!record) {
        return output;
    }
    for (const auto& occurrence : record->occurrences) {
        if (occurrence.role == role) {
            output.insert(occurrence.name);
        }
    }
    return output;
}

void check_removed_references(const symbol_record* base,
                              const symbol_record* removing,
                              const symbol_record* other,
                              structural_merge_result& output) {
    const auto base_definitions = symbol_names(base, symbol_role::definition);
    const auto remaining_definitions = symbol_names(removing, symbol_role::definition);
    const auto other_references = symbol_names(other, symbol_role::reference);
    for (const auto& name : base_definitions) {
        if (!remaining_definitions.contains(name) && other_references.contains(name)) {
            add_reason(output, "reference to symbol removed by the other side: " + name, name);
        }
    }
}

auto apply_hunks(std::span<const std::byte> base,
                 std::vector<merge_hunk> hunks,
                 structural_merge_result& output) -> result<std::vector<std::byte>> {
    std::ranges::sort(hunks, [](const auto& left, const auto& right) {
        return std::tuple{left.start, left.end, left.side, left.name} <
               std::tuple{right.start, right.end, right.side, right.name};
    });
    auto combined = std::vector<merge_hunk>{};
    for (auto& hunk : hunks) {
        if (hunk.start > hunk.end || hunk.end > base.size()) {
            return std::unexpected(
                error{errc::corrupt, "structural merge range is outside the base", hunk.name});
        }
        const auto shared_insertion = !combined.empty() && hunk.start == combined.back().start &&
                                      hunk.start == hunk.end &&
                                      combined.back().start == combined.back().end;
        if (!combined.empty() && hunk.start <= combined.back().end && !shared_insertion) {
            output.outcome = structural_merge_outcome::conflict;
            output.reasons.push_back("overlapping or adjacent structural edits at node " +
                                     hunk.name);
            if (!output.node) {
                output.node = hunk.name;
            }
            continue;
        }
        if (shared_insertion) {
            auto& replacement = combined.back().replacement;
            if (!replacement.empty() && replacement.back() != newline) {
                replacement.push_back(newline);
            }
            replacement.insert(replacement.end(), hunk.replacement.begin(), hunk.replacement.end());
            continue;
        }
        combined.push_back(std::move(hunk));
    }
    if (output.outcome == structural_merge_outcome::conflict) {
        return std::vector<std::byte>{};
    }
    auto merged = std::vector<std::byte>{};
    auto cursor = std::uint64_t{};
    for (const auto& hunk : combined) {
        merged.insert(merged.end(),
                      base.begin() + static_cast<std::ptrdiff_t>(cursor),
                      base.begin() + static_cast<std::ptrdiff_t>(hunk.start));
        auto replacement = hunk.replacement;
        if (hunk.start == hunk.end && !replacement.empty()) {
            const auto before_newline =
                hunk.start != 0 && base[static_cast<std::size_t>(hunk.start - 1)] == newline;
            if (!before_newline && replacement.front() != newline) {
                merged.push_back(newline);
            }
            const auto after_newline =
                hunk.start < base.size() && base[static_cast<std::size_t>(hunk.start)] == newline;
            if (!after_newline && replacement.back() != newline) {
                replacement.push_back(newline);
            }
        }
        merged.insert(merged.end(), replacement.begin(), replacement.end());
        cursor = hunk.end;
    }
    merged.insert(merged.end(), base.begin() + static_cast<std::ptrdiff_t>(cursor), base.end());
    return merged;
}

} // namespace

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

auto has_recovery_node(const parse_node& node) -> bool {
    return node.error || node.missing || std::ranges::any_of(node.children, [](const auto& child) {
               return has_recovery_node(child);
           });
}

auto merge_structural(const parse_tree& base_tree,
                      const parse_tree& left_tree,
                      const parse_tree& right_tree,
                      std::span<const std::byte> base_source,
                      std::span<const std::byte> left_source,
                      std::span<const std::byte> right_source,
                      const symbol_record* base_symbols,
                      const symbol_record* left_symbols,
                      const symbol_record* right_symbols) -> result<structural_merge_result> {
    if (base_tree.language != left_tree.language || base_tree.language != right_tree.language ||
        base_tree.grammar != left_tree.grammar || base_tree.grammar != right_tree.grammar ||
        base_tree.node_types != left_tree.node_types ||
        base_tree.node_types != right_tree.node_types) {
        return std::unexpected(error{errc::invalid_argument,
                                     "structural merge trees use different grammars",
                                     base_tree.language});
    }
    auto output = structural_merge_result{.grammar = base_tree.grammar};
    const auto left_recovery = has_recovery_node(left_tree.root);
    const auto right_recovery = has_recovery_node(right_tree.root);
    if (left_recovery || right_recovery) {
        add_reason(output, "merged syntax contains ERROR or MISSING nodes", left_tree.language);
        const auto source = left_recovery ? left_source : right_source;
        output.proposal.assign(source.begin(), source.end());
        return output;
    }
    const auto base = definitions(base_tree, base_source);
    const auto left = definitions(left_tree, left_source);
    const auto right = definitions(right_tree, right_source);
    auto hunks = std::vector<merge_hunk>{};
    for (const auto& [name, base_unit] : base) {
        const auto left_unit = left.find(name);
        const auto right_unit = right.find(name);
        const auto left_changed = left_unit == left.end() || changed(base_unit, left_unit->second);
        const auto right_changed =
            right_unit == right.end() || changed(base_unit, right_unit->second);
        if (left_changed && right_changed) {
            add_reason(output, "both sides edited definition " + name, name);
            const auto replacement =
                left_unit == left.end() ? std::vector<std::byte>{} : unit_bytes(left_unit->second);
            hunks.push_back({base_unit.value->range.start.byte,
                             base_unit.value->range.end.byte,
                             replacement,
                             0,
                             name});
            continue;
        }
        if (!left_changed && !right_changed) {
            continue;
        }
        const auto selected = left_changed ? left_unit : right_unit;
        const auto replacement = selected == (left_changed ? left.end() : right.end())
                                     ? std::vector<std::byte>{}
                                     : unit_bytes(selected->second);
        hunks.push_back({base_unit.value->range.start.byte,
                         base_unit.value->range.end.byte,
                         replacement,
                         left_changed ? 0U : 1U,
                         name});
    }
    auto add_new_definitions = [&](const parse_tree& side_tree,
                                   const std::map<std::string, unit_view>& side,
                                   const std::map<std::string, unit_view>& other,
                                   unsigned side_number) {
        for (const auto& unit : side_tree.units) {
            if (unit.kind != frontend_unit_kind::definition || base.contains(unit.name)) {
                continue;
            }
            if (other.contains(unit.name)) {
                add_reason(output,
                           "both sides introduced definition with the same name: " + unit.name,
                           unit.name);
                if (side_number != 0) {
                    continue;
                }
            }
            const auto found = side.find(unit.name);
            if (found == side.end()) {
                continue;
            }
            const auto offset = insertion_offset(base_tree, side_tree, unit, base);
            hunks.push_back({offset, offset, unit_bytes(found->second), side_number, unit.name});
        }
    };
    add_new_definitions(left_tree, left, right, 0);
    add_new_definitions(right_tree, right, left, 1);
    if (hunks.empty() && (!std::ranges::equal(base_source, left_source) ||
                          !std::ranges::equal(base_source, right_source))) {
        output.outcome = structural_merge_outcome::conflict;
        output.reasons.push_back("edits are outside independently matchable definitions");
    }
    check_removed_references(base_symbols, left_symbols, right_symbols, output);
    check_removed_references(base_symbols, right_symbols, left_symbols, output);
    auto proposal = apply_hunks(base_source, std::move(hunks), output);
    if (!proposal) {
        return std::unexpected(proposal.error());
    }
    output.proposal = std::move(*proposal);
    return output;
}

} // namespace liquid::repository_detail
