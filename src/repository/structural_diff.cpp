#include "repository/structural_diff.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace liquid::repository_detail {
namespace {

struct node_value {
    const parse_node* node{};
    const parse_node* parent{};
    std::string kind;
    std::string fingerprint;
    bool comment{};
};

auto source_text(std::span<const std::byte> source, const frontend_source_range& range)
    -> std::string_view {
    if (range.start.byte > range.end.byte || range.end.byte > source.size()) {
        return {};
    }
    return {reinterpret_cast<const char*>(source.data() + range.start.byte),
            static_cast<std::size_t>(range.end.byte - range.start.byte)};
}

auto is_comment(std::string_view kind) -> bool {
    return kind.find("comment") != std::string_view::npos;
}

void append_normalized(std::string& output, std::string_view value) {
    for (const auto character : value) {
        if (!std::isspace(static_cast<unsigned char>(character))) {
            output.push_back(character);
        }
    }
}

void append_collapsed(std::string& output, std::string_view value) {
    auto whitespace = false;
    for (const auto character : value) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            whitespace = !output.empty();
            continue;
        }
        if (whitespace) {
            output.push_back(' ');
            whitespace = false;
        }
        output.push_back(character);
    }
}

auto node_kind(const parse_tree& tree, const parse_node& node) -> std::string_view {
    return node.kind < tree.node_kinds.size() ? tree.node_kinds[node.kind] : std::string_view{};
}

auto fingerprint(const parse_tree& tree,
                 const parse_node& node,
                 std::span<const std::byte> source,
                 std::unordered_map<const parse_node*, std::string>& cache) -> result<std::string> {
    if (const auto found = cache.find(&node); found != cache.end()) {
        return found->second;
    }
    auto value = std::string{node_kind(tree, node)};
    value.push_back('\0');
    auto cursor = node.range.start.byte;
    auto semantic_children = std::size_t{};
    for (const auto& child : node.children) {
        if (cursor < child.range.start.byte && child.range.start.byte <= source.size()) {
            append_normalized(
                value, source_text(source, {{cursor, 1, 1}, {child.range.start.byte, 1, 1}}));
        }
        cursor = std::max(cursor, child.range.end.byte);
        if (is_comment(node_kind(tree, child))) {
            continue;
        }
        auto child_fingerprint = fingerprint(tree, child, source, cache);
        if (!child_fingerprint) {
            return std::unexpected(child_fingerprint.error());
        }
        value.append(*child_fingerprint);
        value.push_back('\0');
        ++semantic_children;
    }
    if (cursor < node.range.end.byte && node.range.end.byte <= source.size()) {
        append_normalized(value,
                          source_text(source, {{cursor, 1, 1}, {node.range.end.byte, 1, 1}}));
    }
    if (semantic_children == 0 && node.children.empty()) {
        const auto kind = node_kind(tree, node);
        if (kind == "text" || kind == "raw_text" || kind == "shell_text") {
            append_collapsed(value, source_text(source, node.range));
        } else {
            value.append(source_text(source, node.range));
        }
    }
    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    auto digest = hash(algorithm::blake3, bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    auto encoded = to_string(*digest);
    cache.emplace(&node, encoded);
    return encoded;
}

auto flatten(const parse_tree& tree, std::span<const std::byte> source)
    -> result<std::vector<node_value>> {
    auto output = std::vector<node_value>{};
    auto fingerprints = std::unordered_map<const parse_node*, std::string>{};
    auto visit =
        [&](auto&& self, const parse_node& node, const parse_node* parent) -> result<void> {
        auto value = fingerprint(tree, node, source, fingerprints);
        if (!value) {
            return std::unexpected(value.error());
        }
        const auto kind = std::string{node_kind(tree, node)};
        output.push_back({&node, parent, kind, std::move(*value), is_comment(kind)});
        for (const auto& child : node.children) {
            auto added = self(self, child, &node);
            if (!added) {
                return std::unexpected(added.error());
            }
        }
        return {};
    };
    auto added = visit(visit, tree.root, nullptr);
    if (!added) {
        return std::unexpected(added.error());
    }
    return output;
}

auto by_node(std::span<const node_value> values)
    -> std::unordered_map<const parse_node*, const node_value*> {
    auto output = std::unordered_map<const parse_node*, const node_value*>{};
    for (const auto& value : values) {
        output.emplace(value.node, &value);
    }
    return output;
}

auto unit_for_node(const parse_tree& tree, const parse_node& node) -> const frontend_unit* {
    const auto found = std::ranges::find_if(tree.units, [&](const auto& unit) {
        return unit.range.start.byte == node.range.start.byte &&
               unit.range.end.byte == node.range.end.byte;
    });
    return found == tree.units.end() ? nullptr : &*found;
}

auto leaf_name(const parse_tree& tree, const parse_node& node, std::span<const std::byte> source)
    -> std::optional<std::string> {
    if (const auto* unit = unit_for_node(tree, node)) {
        return unit->name;
    }
    for (const auto& child : node.children) {
        if (child.field && (*child.field == "name" || *child.field == "declarator")) {
            auto nested = leaf_name(tree, child, source);
            if (nested) {
                return nested;
            }
        }
    }
    if (node.children.empty()) {
        const auto kind = node_kind(tree, node);
        if (kind.find("identifier") != std::string_view::npos) {
            return std::string{source_text(source, node.range)};
        }
    }
    return std::nullopt;
}

using matches = std::unordered_map<const parse_node*, const parse_node*>;

void match_units(const parse_tree& old_tree,
                 const parse_tree& new_tree,
                 std::span<const node_value> old_nodes,
                 std::span<const node_value> new_nodes,
                 matches& old_to_new,
                 matches& new_to_old) {
    for (const auto& old_unit : old_tree.units) {
        const auto new_unit = std::ranges::find_if(new_tree.units, [&](const auto& candidate) {
            return candidate.fingerprint.alg == old_unit.fingerprint.alg &&
                   candidate.fingerprint.bytes == old_unit.fingerprint.bytes;
        });
        if (new_unit == new_tree.units.end()) {
            continue;
        }
        const auto old_node = std::ranges::find_if(old_nodes, [&](const auto& value) {
            return value.node->range.start.byte == old_unit.range.start.byte &&
                   value.node->range.end.byte == old_unit.range.end.byte;
        });
        const auto new_node = std::ranges::find_if(new_nodes, [&](const auto& value) {
            return value.node->range.start.byte == new_unit->range.start.byte &&
                   value.node->range.end.byte == new_unit->range.end.byte;
        });
        if (old_node != old_nodes.end() && new_node != new_nodes.end()) {
            old_to_new.emplace(old_node->node, new_node->node);
            new_to_old.emplace(new_node->node, old_node->node);
        }
    }
}

void match_identical(std::span<const node_value> old_nodes,
                     std::span<const node_value> new_nodes,
                     matches& old_to_new,
                     matches& new_to_old) {
    auto old_hashes = std::map<std::string, std::vector<const parse_node*>>{};
    auto new_hashes = std::map<std::string, std::vector<const parse_node*>>{};
    for (const auto& value : old_nodes) {
        if (!value.comment && !old_to_new.contains(value.node)) {
            old_hashes[value.fingerprint].push_back(value.node);
        }
    }
    for (const auto& value : new_nodes) {
        if (!value.comment && !new_to_old.contains(value.node)) {
            new_hashes[value.fingerprint].push_back(value.node);
        }
    }
    for (const auto& [hash_value, old_values] : old_hashes) {
        const auto found = new_hashes.find(hash_value);
        if (old_values.size() == 1 && found != new_hashes.end() && found->second.size() == 1) {
            old_to_new.emplace(old_values.front(), found->second.front());
            new_to_old.emplace(found->second.front(), old_values.front());
        }
    }
}

void match_containers(std::span<const node_value> old_nodes,
                      std::span<const node_value> new_nodes,
                      matches& old_to_new,
                      matches& new_to_old) {
    auto candidates = std::unordered_map<std::string, std::vector<const node_value*>>{};
    for (const auto& value : new_nodes) {
        if (!value.comment && !value.node->children.empty()) {
            candidates[value.kind].push_back(&value);
        }
    }
    for (auto old_index = old_nodes.size(); old_index != 0; --old_index) {
        const auto& old_value = old_nodes[old_index - 1];
        if (old_value.comment || old_value.node->children.empty() ||
            old_to_new.contains(old_value.node)) {
            continue;
        }
        const auto candidates_for_kind = candidates.find(old_value.kind);
        if (candidates_for_kind == candidates.end()) {
            continue;
        }
        const parse_node* best{};
        auto best_children = std::size_t{};
        for (const auto* new_value : candidates_for_kind->second) {
            if (new_to_old.contains(new_value->node)) {
                continue;
            }
            auto children = std::size_t{};
            for (const auto& old_child : old_value.node->children) {
                const auto matched = old_to_new.find(&old_child);
                if (matched != old_to_new.end() &&
                    std::ranges::find_if(new_value->node->children, [&](const auto& child) {
                        return &child == matched->second;
                    }) != new_value->node->children.end()) {
                    ++children;
                }
            }
            if (children > best_children) {
                best = new_value->node;
                best_children = children;
            }
        }
        if (best) {
            old_to_new.emplace(old_value.node, best);
            new_to_old.emplace(best, old_value.node);
        }
    }
}

void match_children(const std::unordered_map<const parse_node*, const node_value*>& old_values,
                    const std::unordered_map<const parse_node*, const node_value*>& new_values,
                    matches& old_to_new,
                    matches& new_to_old) {
    auto changed = true;
    while (changed) {
        changed = false;
        auto parents = std::vector<std::pair<const parse_node*, const parse_node*>>{};
        parents.reserve(old_to_new.size());
        for (const auto& match : old_to_new) {
            parents.push_back(match);
        }
        for (const auto& [old_parent, new_parent] : parents) {
            auto new_index = std::size_t{};
            for (const auto& old_child : old_parent->children) {
                if (old_values.at(&old_child)->comment || old_to_new.contains(&old_child)) {
                    continue;
                }
                const auto compatible = [&](const parse_node& candidate) {
                    return !new_values.at(&candidate)->comment &&
                           !new_to_old.contains(&candidate) &&
                           old_values.at(&old_child)->kind == new_values.at(&candidate)->kind &&
                           old_child.field == candidate.field;
                };
                auto candidate_index = new_parent->children.size();
                for (auto index = new_index; index < new_parent->children.size(); ++index) {
                    const auto& candidate = new_parent->children[index];
                    if (compatible(candidate) && old_values.at(&old_child)->fingerprint ==
                                                     new_values.at(&candidate)->fingerprint) {
                        candidate_index = index;
                        break;
                    }
                    if (candidate_index == new_parent->children.size() && compatible(candidate)) {
                        candidate_index = index;
                    }
                }
                if (candidate_index == new_parent->children.size()) {
                    continue;
                }
                const auto* candidate = &new_parent->children[candidate_index];
                old_to_new.emplace(&old_child, candidate);
                new_to_old.emplace(candidate, &old_child);
                new_index = candidate_index + 1;
                changed = true;
            }
        }
    }
}

auto moved_units(const parse_tree& old_tree, const parse_tree& new_tree) -> std::set<std::string> {
    auto old_names = std::vector<std::string>{};
    auto new_names = std::vector<std::string>{};
    for (const auto& unit : old_tree.units) {
        if (unit.kind == frontend_unit_kind::definition) {
            old_names.push_back(unit.name);
        }
    }
    for (const auto& unit : new_tree.units) {
        if (unit.kind == frontend_unit_kind::definition) {
            new_names.push_back(unit.name);
        }
    }
    auto cells = std::vector<std::size_t>((old_names.size() + 1) * (new_names.size() + 1));
    const auto columns = new_names.size() + 1;
    for (auto old_index = old_names.size(); old_index != 0; --old_index) {
        for (auto new_index = new_names.size(); new_index != 0; --new_index) {
            const auto row = old_index - 1;
            const auto column = new_index - 1;
            cells[row * columns + column] = old_names[row] == new_names[column]
                                                ? cells[(row + 1) * columns + column + 1] + 1
                                                : std::max(cells[(row + 1) * columns + column],
                                                           cells[row * columns + column + 1]);
        }
    }
    auto stable = std::set<std::string>{};
    auto old_index = std::size_t{};
    auto new_index = std::size_t{};
    while (old_index < old_names.size() && new_index < new_names.size()) {
        if (old_names[old_index] == new_names[new_index]) {
            stable.insert(old_names[old_index]);
            ++old_index;
            ++new_index;
        } else if (cells[(old_index + 1) * columns + new_index] >=
                   cells[old_index * columns + new_index + 1]) {
            ++old_index;
        } else {
            ++new_index;
        }
    }
    auto moved = std::set<std::string>{};
    for (const auto& name : old_names) {
        if (std::ranges::find(new_names, name) != new_names.end() && !stable.contains(name)) {
            moved.insert(name);
        }
    }
    return moved;
}

auto reordered_nodes(const matches& old_to_new, const matches& new_to_old)
    -> std::set<const parse_node*> {
    auto moved = std::set<const parse_node*>{};
    for (const auto& [old_parent, new_parent] : old_to_new) {
        auto old_children = std::vector<const parse_node*>{};
        auto new_children = std::vector<const parse_node*>{};
        for (const auto& child : old_parent->children) {
            const auto matched = old_to_new.find(&child);
            if (matched != old_to_new.end() &&
                std::ranges::find_if(new_parent->children, [&](const auto& candidate) {
                    return &candidate == matched->second;
                }) != new_parent->children.end()) {
                old_children.push_back(&child);
            }
        }
        for (const auto& child : new_parent->children) {
            const auto matched = new_to_old.find(&child);
            if (matched != new_to_old.end() &&
                std::ranges::find(old_children, matched->second) != old_children.end()) {
                new_children.push_back(matched->second);
            }
        }
        const auto columns = new_children.size() + 1;
        auto cells = std::vector<std::size_t>((old_children.size() + 1) * columns);
        for (auto old_index = old_children.size(); old_index != 0; --old_index) {
            for (auto new_index = new_children.size(); new_index != 0; --new_index) {
                const auto row = old_index - 1;
                const auto column = new_index - 1;
                cells[row * columns + column] = old_children[row] == new_children[column]
                                                    ? cells[(row + 1) * columns + column + 1] + 1
                                                    : std::max(cells[(row + 1) * columns + column],
                                                               cells[row * columns + column + 1]);
            }
        }
        auto stable = std::set<const parse_node*>{};
        auto old_index = std::size_t{};
        auto new_index = std::size_t{};
        while (old_index < old_children.size() && new_index < new_children.size()) {
            if (old_children[old_index] == new_children[new_index]) {
                stable.insert(old_children[old_index]);
                ++old_index;
                ++new_index;
            } else if (cells[(old_index + 1) * columns + new_index] >=
                       cells[old_index * columns + new_index + 1]) {
                ++old_index;
            } else {
                ++new_index;
            }
        }
        for (const auto* child : old_children) {
            if (!stable.contains(child)) {
                moved.insert(child);
            }
        }
    }
    return moved;
}

auto move_candidate(const parse_tree& tree, const parse_node& node) -> bool {
    if (unit_for_node(tree, node)) {
        return true;
    }
    const auto kind = node_kind(tree, node);
    return kind.find("function") != std::string_view::npos ||
           kind.find("method") != std::string_view::npos;
}

void append_comment_edits(std::span<const std::byte> old_source,
                          std::span<const std::byte> new_source,
                          std::span<const node_value> old_nodes,
                          std::span<const node_value> new_nodes,
                          structural_diff& output) {
    auto old_comments = std::vector<const node_value*>{};
    auto new_comments = std::vector<const node_value*>{};
    for (const auto& value : old_nodes) {
        if (value.comment) {
            old_comments.push_back(&value);
        }
    }
    for (const auto& value : new_nodes) {
        if (value.comment) {
            new_comments.push_back(&value);
        }
    }
    auto matched = std::vector<bool>(new_comments.size());
    auto unmatched_old = std::vector<const node_value*>{};
    for (const auto* old_comment : old_comments) {
        const auto text = source_text(old_source, old_comment->node->range);
        auto found = new_comments.size();
        for (auto index = std::size_t{}; index < new_comments.size(); ++index) {
            if (!matched[index] &&
                source_text(new_source, new_comments[index]->node->range) == text) {
                found = index;
                break;
            }
        }
        if (found == new_comments.size()) {
            unmatched_old.push_back(old_comment);
        } else {
            matched[found] = true;
        }
    }
    auto unmatched_new = std::vector<const node_value*>{};
    for (auto index = std::size_t{}; index < new_comments.size(); ++index) {
        if (!matched[index]) {
            unmatched_new.push_back(new_comments[index]);
        }
    }
    const auto count = std::max(unmatched_old.size(), unmatched_new.size());
    for (auto index = std::size_t{}; index < count; ++index) {
        const auto* old_comment = index < unmatched_old.size() ? unmatched_old[index] : nullptr;
        const auto* new_comment = index < unmatched_new.size() ? unmatched_new[index] : nullptr;
        output.edits.push_back({
            .kind = structural_edit_kind::comment_changed,
            .node_kind = old_comment ? old_comment->kind : new_comment->kind,
            .old_range = old_comment ? std::optional{old_comment->node->range} : std::nullopt,
            .new_range = new_comment ? std::optional{new_comment->node->range} : std::nullopt,
        });
    }
}

} // namespace

auto compare_structural(const parse_tree& old_tree,
                        const parse_tree& new_tree,
                        std::span<const std::byte> old_source,
                        std::span<const std::byte> new_source) -> result<structural_diff> {
    if (old_tree.language != new_tree.language || old_tree.grammar != new_tree.grammar ||
        old_tree.node_types != new_tree.node_types) {
        return std::unexpected(error{errc::invalid_argument,
                                     "structural trees use different grammars",
                                     old_tree.language + " / " + new_tree.language});
    }
    auto old_nodes = flatten(old_tree, old_source);
    auto new_nodes = flatten(new_tree, new_source);
    if (!old_nodes) {
        return std::unexpected(old_nodes.error());
    }
    if (!new_nodes) {
        return std::unexpected(new_nodes.error());
    }
    auto old_values = by_node(*old_nodes);
    auto new_values = by_node(*new_nodes);
    auto old_to_new = matches{{&old_tree.root, &new_tree.root}};
    auto new_to_old = matches{{&new_tree.root, &old_tree.root}};
    match_units(old_tree, new_tree, *old_nodes, *new_nodes, old_to_new, new_to_old);
    match_identical(*old_nodes, *new_nodes, old_to_new, new_to_old);
    match_containers(*old_nodes, *new_nodes, old_to_new, new_to_old);
    match_children(old_values, new_values, old_to_new, new_to_old);

    auto output = structural_diff{.language = old_tree.language, .grammar = old_tree.grammar};
    const auto moved = moved_units(old_tree, new_tree);
    const auto reordered = reordered_nodes(old_to_new, new_to_old);
    for (const auto& old_value : *old_nodes) {
        if (old_value.comment) {
            continue;
        }
        const auto found = old_to_new.find(old_value.node);
        if (found == old_to_new.end()) {
            if (!old_value.parent || old_to_new.contains(old_value.parent)) {
                output.edits.push_back({.kind = structural_edit_kind::removed,
                                        .node_kind = old_value.kind,
                                        .name = leaf_name(old_tree, *old_value.node, old_source),
                                        .old_range = old_value.node->range});
            }
            continue;
        }
        const auto* new_node = found->second;
        const auto* new_value = new_values.at(new_node);
        if (const auto* unit = unit_for_node(old_tree, *old_value.node);
            unit && moved.contains(unit->name)) {
            output.edits.push_back({.kind = structural_edit_kind::moved,
                                    .node_kind = old_value.kind,
                                    .name = unit->name,
                                    .old_range = old_value.node->range,
                                    .new_range = new_node->range});
            continue;
        }
        const auto ancestor_reordered = old_value.parent && reordered.contains(old_value.parent);
        if (reordered.contains(old_value.node) && !ancestor_reordered &&
            move_candidate(old_tree, *old_value.node)) {
            output.edits.push_back({.kind = structural_edit_kind::moved,
                                    .node_kind = old_value.kind,
                                    .name = leaf_name(old_tree, *old_value.node, old_source),
                                    .old_range = old_value.node->range,
                                    .new_range = new_node->range});
            continue;
        }
        const auto parent_moved = old_value.parent && new_value->parent &&
                                  old_to_new.contains(old_value.parent) &&
                                  old_to_new.at(old_value.parent) != new_value->parent;
        if (parent_moved && move_candidate(old_tree, *old_value.node)) {
            output.edits.push_back({.kind = structural_edit_kind::moved,
                                    .node_kind = old_value.kind,
                                    .name = leaf_name(old_tree, *old_value.node, old_source),
                                    .old_range = old_value.node->range,
                                    .new_range = new_node->range});
            continue;
        }
        if (old_value.fingerprint == new_value->fingerprint) {
            continue;
        }
        if (std::ranges::any_of(old_value.node->children,
                                [&](const auto& child) { return reordered.contains(&child); })) {
            continue;
        }
        auto changed_child = false;
        for (const auto& child : old_value.node->children) {
            const auto child_match = old_to_new.find(&child);
            if (child_match == old_to_new.end()) {
                changed_child = true;
                break;
            }
            if (old_values.at(&child)->fingerprint !=
                new_values.at(child_match->second)->fingerprint) {
                changed_child = true;
                break;
            }
        }
        if (!changed_child) {
            output.edits.push_back({.kind = structural_edit_kind::modified,
                                    .node_kind = old_value.kind,
                                    .name = leaf_name(old_tree, *old_value.node, old_source),
                                    .old_range = old_value.node->range,
                                    .new_range = new_node->range});
        }
    }
    for (const auto& new_value : *new_nodes) {
        if (new_value.comment || new_to_old.contains(new_value.node)) {
            continue;
        }
        if (!new_value.parent || new_to_old.contains(new_value.parent)) {
            output.edits.push_back({.kind = structural_edit_kind::added,
                                    .node_kind = new_value.kind,
                                    .name = leaf_name(new_tree, *new_value.node, new_source),
                                    .new_range = new_value.node->range});
        }
    }
    append_comment_edits(old_source, new_source, *old_nodes, *new_nodes, output);
    output.formatting_only =
        old_source.size() != new_source.size() || !std::ranges::equal(old_source, new_source);
    output.formatting_only = output.formatting_only && output.edits.empty();
    std::ranges::sort(output.edits, [](const auto& left, const auto& right) {
        const auto left_byte =
            left.new_range ? left.new_range->start.byte : left.old_range->start.byte;
        const auto right_byte =
            right.new_range ? right.new_range->start.byte : right.old_range->start.byte;
        return std::tuple{left_byte, left.kind, left.node_kind} <
               std::tuple{right_byte, right.kind, right.node_kind};
    });
    return output;
}

} // namespace liquid::repository_detail
