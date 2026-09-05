#include "liquid/repository.hpp"

#include "repository/diff.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>

namespace liquid {
namespace {

constexpr auto maximum_lcs_cells = std::size_t{4'000'000};

struct flat_tree_entry {
    std::string path;
    tree_entry value;
};

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto same_digests(std::span<const digest> left, std::span<const digest> right) -> bool {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const digest& first, const digest& second) {
               return same_digest(first, second);
           });
}

auto same_nested(const std::optional<repo_ref>& left, const std::optional<repo_ref>& right)
    -> bool {
    if (!left || !right) {
        return !left && !right;
    }
    return left->repo_id == right->repo_id && same_digest(left->id.value, right->id.value);
}

auto empty_node(const node_id& value) -> bool {
    return std::ranges::all_of(value, [](std::byte item) { return item == std::byte{}; });
}

auto diff_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto flatten(repository& source,
             const liquid::tree& root,
             std::string prefix,
             std::vector<digest> ancestors,
             std::vector<flat_tree_entry>& output) -> runtime::task<void> {
    for (const auto& entry : root.entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        output.push_back({path, entry});
        if (entry.kind != node_kind::tree) {
            continue;
        }
        if (std::ranges::any_of(
                ancestors, [&](const digest& value) { return same_digest(value, entry.ref); })) {
            co_return std::unexpected(
                diff_error(errc::corrupt, "tree contains a reference cycle", path));
        }
        auto child = co_await source.tree(entry.ref);
        if (!child) {
            co_return std::unexpected(child.error());
        }
        auto child_ancestors = ancestors;
        child_ancestors.push_back(entry.ref);
        auto nested = co_await flatten(source, *child, path, std::move(child_ancestors), output);
        if (!nested) {
            co_return std::unexpected(nested.error());
        }
    }
    co_return result<void>{};
}

auto text_bytes(std::span<const std::byte> bytes) -> bool {
    for (auto index = std::size_t{}; index < bytes.size();) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        if (value < 0x80) {
            if (value != '\n' && value != '\r' && value != '\t' && value < 0x20) {
                return false;
            }
            ++index;
            continue;
        }
        auto length = std::size_t{};
        auto minimum_second = 0x80U;
        auto maximum_second = 0xbfU;
        if (value >= 0xc2 && value <= 0xdf) {
            length = 2;
        } else if (value >= 0xe0 && value <= 0xef) {
            length = 3;
            if (value == 0xe0) {
                minimum_second = 0xa0;
            } else if (value == 0xed) {
                maximum_second = 0x9f;
            }
        } else if (value >= 0xf0 && value <= 0xf4) {
            length = 4;
            if (value == 0xf0) {
                minimum_second = 0x90;
            } else if (value == 0xf4) {
                maximum_second = 0x8f;
            }
        } else {
            return false;
        }
        if (index + length > bytes.size()) {
            return false;
        }
        const auto second = std::to_integer<unsigned char>(bytes[index + 1]);
        if (second < minimum_second || second > maximum_second) {
            return false;
        }
        for (auto continuation = std::size_t{2}; continuation < length; ++continuation) {
            const auto byte = std::to_integer<unsigned char>(bytes[index + continuation]);
            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

auto lines(std::span<const std::byte> bytes) -> std::vector<std::string_view> {
    const auto source = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    auto output = std::vector<std::string_view>{};
    auto offset = std::size_t{};
    while (offset < source.size()) {
        const auto newline = source.find('\n', offset);
        const auto length =
            newline == std::string_view::npos ? source.size() - offset : newline - offset;
        output.push_back(source.substr(offset, length));
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return output;
}

auto coarse_hunk(std::span<const std::string_view> old_lines,
                 std::span<const std::string_view> new_lines) -> std::vector<line_change> {
    auto prefix = std::size_t{};
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    auto suffix = std::size_t{};
    while (suffix < old_lines.size() - prefix && suffix < new_lines.size() - prefix &&
           old_lines[old_lines.size() - suffix - 1] == new_lines[new_lines.size() - suffix - 1]) {
        ++suffix;
    }
    if (prefix == old_lines.size() && prefix == new_lines.size()) {
        return {};
    }
    return {{prefix + 1,
             old_lines.size() - prefix - suffix,
             prefix + 1,
             new_lines.size() - prefix - suffix}};
}

auto line_hunks(std::span<const std::byte> old_bytes, std::span<const std::byte> new_bytes)
    -> std::optional<std::vector<line_change>> {
    if (!text_bytes(old_bytes) || !text_bytes(new_bytes)) {
        return std::nullopt;
    }
    const auto old_lines = lines(old_bytes);
    const auto new_lines = lines(new_bytes);
    if (!old_lines.empty() &&
        new_lines.size() > std::numeric_limits<std::size_t>::max() / old_lines.size()) {
        return coarse_hunk(old_lines, new_lines);
    }
    const auto cells = old_lines.size() * new_lines.size();
    if (cells > maximum_lcs_cells) {
        return coarse_hunk(old_lines, new_lines);
    }
    const auto columns = new_lines.size() + 1;
    auto lcs = std::vector<std::uint32_t>((old_lines.size() + 1) * columns);
    for (auto old_index = old_lines.size(); old_index != 0; --old_index) {
        for (auto new_index = new_lines.size(); new_index != 0; --new_index) {
            const auto row = old_index - 1;
            const auto column = new_index - 1;
            const auto cell = row * columns + column;
            if (old_lines[row] == new_lines[column]) {
                lcs[cell] = lcs[(row + 1) * columns + column + 1] + 1;
            } else {
                lcs[cell] =
                    std::max(lcs[(row + 1) * columns + column], lcs[row * columns + column + 1]);
            }
        }
    }
    auto output = std::vector<line_change>{};
    auto old_index = std::size_t{};
    auto new_index = std::size_t{};
    while (old_index < old_lines.size() || new_index < new_lines.size()) {
        if (old_index < old_lines.size() && new_index < new_lines.size() &&
            old_lines[old_index] == new_lines[new_index]) {
            ++old_index;
            ++new_index;
            continue;
        }
        auto hunk = line_change{old_index + 1, 0, new_index + 1, 0};
        while (old_index < old_lines.size() || new_index < new_lines.size()) {
            if (old_index < old_lines.size() && new_index < new_lines.size() &&
                old_lines[old_index] == new_lines[new_index]) {
                break;
            }
            const auto remove =
                old_index < old_lines.size() &&
                (new_index == new_lines.size() || lcs[(old_index + 1) * columns + new_index] >=
                                                      lcs[old_index * columns + new_index + 1]);
            if (remove) {
                ++old_index;
                ++hunk.old_count;
            } else {
                ++new_index;
                ++hunk.new_count;
            }
        }
        output.push_back(hunk);
    }
    return output;
}

auto has_change(const tree_diff_entry& entry, tree_diff_kind kind) -> bool {
    return std::ranges::find(entry.changes, kind) != entry.changes.end();
}

void add_change(tree_diff_entry& entry, tree_diff_kind kind) {
    if (!has_change(entry, kind)) {
        entry.changes.push_back(kind);
    }
}

auto blob_id(const tree_entry& entry) -> std::optional<digest> {
    return entry.kind == node_kind::blob ? std::optional<digest>{entry.ref} : std::nullopt;
}

auto make_removed(const flat_tree_entry& old_entry) -> tree_diff_entry {
    return {.path = old_entry.path,
            .node = old_entry.value.node,
            .kind = old_entry.value.kind,
            .changes = {tree_diff_kind::removed},
            .old_mode = old_entry.value.mode,
            .old_blob = blob_id(old_entry.value)};
}

auto make_added(const flat_tree_entry& new_entry) -> tree_diff_entry {
    return {.path = new_entry.path,
            .node = new_entry.value.node,
            .kind = new_entry.value.kind,
            .changes = {tree_diff_kind::added},
            .new_mode = new_entry.value.mode,
            .new_blob = blob_id(new_entry.value)};
}

auto changed_entry(const flat_tree_entry& old_entry, const flat_tree_entry& new_entry)
    -> std::optional<tree_diff_entry> {
    auto output = tree_diff_entry{.path = new_entry.path,
                                  .node = new_entry.value.node,
                                  .kind = new_entry.value.kind,
                                  .old_mode = old_entry.value.mode,
                                  .new_mode = new_entry.value.mode,
                                  .old_blob = blob_id(old_entry.value),
                                  .new_blob = blob_id(new_entry.value)};
    if (old_entry.path != new_entry.path) {
        output.old_path = old_entry.path;
        add_change(output, tree_diff_kind::moved);
    }
    if (old_entry.value.mode != new_entry.value.mode) {
        add_change(output, tree_diff_kind::mode_changed);
    }
    const auto reference_changed = !same_digest(old_entry.value.ref, new_entry.value.ref);
    const auto nested_changed = !same_nested(old_entry.value.nested, new_entry.value.nested);
    if (old_entry.value.kind != new_entry.value.kind ||
        (new_entry.value.kind != node_kind::tree && (reference_changed || nested_changed))) {
        add_change(output, tree_diff_kind::modified);
    }
    return output.changes.empty() ? std::nullopt : std::optional<tree_diff_entry>{output};
}

auto conflict_map(std::span<const conflict> values) -> std::map<std::string, std::vector<digest>> {
    auto output = std::map<std::string, std::vector<digest>>{};
    for (const auto& value : values) {
        output[value.path] = value.refs;
    }
    return output;
}

void apply_conflicts(std::vector<tree_diff_entry>& entries,
                     std::span<const conflict> old_conflicts,
                     std::span<const conflict> new_conflicts) {
    const auto old_values = conflict_map(old_conflicts);
    const auto new_values = conflict_map(new_conflicts);
    auto paths = std::set<std::string>{};
    for (const auto& [path, unused] : old_values) {
        static_cast<void>(unused);
        paths.insert(path);
    }
    for (const auto& [path, unused] : new_values) {
        static_cast<void>(unused);
        paths.insert(path);
    }
    for (const auto& path : paths) {
        const auto old_found = old_values.find(path);
        const auto new_found = new_values.find(path);
        const auto old_refs =
            old_found == old_values.end() ? std::vector<digest>{} : old_found->second;
        const auto new_refs =
            new_found == new_values.end() ? std::vector<digest>{} : new_found->second;
        if (same_digests(old_refs, new_refs)) {
            continue;
        }
        auto found = std::ranges::find(entries, path, &tree_diff_entry::path);
        if (found == entries.end()) {
            entries.push_back({.path = path});
            found = std::prev(entries.end());
        }
        add_change(*found, tree_diff_kind::conflicted);
        found->old_conflicts = old_refs;
        found->new_conflicts = new_refs;
    }
}

auto attach_hunks(repository& source, std::vector<tree_diff_entry>& entries)
    -> runtime::task<void> {
    for (auto& entry : entries) {
        const auto content_changed = has_change(entry, tree_diff_kind::added) ||
                                     has_change(entry, tree_diff_kind::removed) ||
                                     has_change(entry, tree_diff_kind::modified);
        if (!content_changed || (!entry.old_blob && !entry.new_blob)) {
            continue;
        }
        auto old_bytes = std::vector<std::byte>{};
        if (entry.old_blob) {
            auto loaded = co_await source.blob(*entry.old_blob);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            old_bytes = std::move(*loaded);
        }
        auto new_bytes = std::vector<std::byte>{};
        if (entry.new_blob) {
            auto loaded = co_await source.blob(*entry.new_blob);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            new_bytes = std::move(*loaded);
        }
        entry.line_hunks = line_hunks(old_bytes, new_bytes);
    }
    co_return result<void>{};
}

auto compare(repository& source,
             liquid::tree old_tree,
             liquid::tree new_tree,
             tree_diff_options options) -> runtime::task<std::vector<tree_diff_entry>> {
    auto old_entries = std::vector<flat_tree_entry>{};
    auto old_flattened = co_await flatten(source, old_tree, {}, {}, old_entries);
    if (!old_flattened) {
        co_return std::unexpected(old_flattened.error());
    }
    auto new_entries = std::vector<flat_tree_entry>{};
    auto new_flattened = co_await flatten(source, new_tree, {}, {}, new_entries);
    if (!new_flattened) {
        co_return std::unexpected(new_flattened.error());
    }

    auto new_nodes = std::map<node_id, std::size_t>{};
    auto new_paths = std::map<std::string, std::size_t>{};
    for (auto index = std::size_t{}; index != new_entries.size(); ++index) {
        if (!empty_node(new_entries[index].value.node)) {
            new_nodes.try_emplace(new_entries[index].value.node, index);
        } else {
            new_paths.try_emplace(new_entries[index].path, index);
        }
    }
    auto matched = std::vector<bool>(new_entries.size());
    auto output = std::vector<tree_diff_entry>{};
    for (const auto& old_entry : old_entries) {
        auto new_index = std::optional<std::size_t>{};
        if (empty_node(old_entry.value.node)) {
            const auto found = new_paths.find(old_entry.path);
            if (found != new_paths.end()) {
                new_index = found->second;
            }
        } else {
            const auto found = new_nodes.find(old_entry.value.node);
            if (found != new_nodes.end()) {
                new_index = found->second;
            }
        }
        if (!new_index) {
            output.push_back(make_removed(old_entry));
            continue;
        }
        matched[*new_index] = true;
        auto changed = changed_entry(old_entry, new_entries[*new_index]);
        if (changed) {
            output.push_back(std::move(*changed));
        }
    }
    for (auto index = std::size_t{}; index != new_entries.size(); ++index) {
        if (!matched[index]) {
            output.push_back(make_added(new_entries[index]));
        }
    }
    std::ranges::sort(output, {}, &tree_diff_entry::path);
    if (options.text) {
        auto attached = co_await attach_hunks(source, output);
        if (!attached) {
            co_return std::unexpected(attached.error());
        }
    }
    co_return output;
}

auto anchor_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto hunk_end(const line_change& hunk) -> result<std::uint64_t> {
    if (hunk.old_count > std::numeric_limits<std::uint64_t>::max() - hunk.old_line) {
        return std::unexpected(anchor_error(errc::corrupt, "line hunk range overflows"));
    }
    return hunk.old_line + hunk.old_count;
}

auto add_count(std::uint64_t& total, std::uint64_t count) -> result<void> {
    if (count > std::numeric_limits<std::uint64_t>::max() - total) {
        return std::unexpected(anchor_error(errc::corrupt, "line hunk count overflows"));
    }
    total += count;
    return {};
}

auto shifted_line(std::uint64_t line, std::uint64_t added, std::uint64_t removed)
    -> result<std::uint64_t> {
    if (added >= removed) {
        const auto shift = added - removed;
        if (shift > std::numeric_limits<std::uint64_t>::max() - line) {
            return std::unexpected(anchor_error(errc::corrupt, "mapped line overflows"));
        }
        return line + shift;
    }
    const auto shift = removed - added;
    if (shift >= line) {
        return std::unexpected(anchor_error(errc::corrupt, "mapped line underflows"));
    }
    return line - shift;
}

auto map_hunks(std::span<const text_anchor> anchors,
               std::span<const std::size_t> indices,
               const tree_diff_entry& entry,
               std::vector<mapped_anchor>& output) -> result<void> {
    if (!entry.line_hunks) {
        return std::unexpected(anchor_error(
            errc::unsupported, "text anchor cannot map through non-text content", entry.path));
    }
    auto sorted = std::vector<std::size_t>{indices.begin(), indices.end()};
    std::ranges::sort(sorted, {}, [&](std::size_t index) { return anchors[index].line; });
    auto hunk_index = std::size_t{};
    auto added = std::uint64_t{};
    auto removed = std::uint64_t{};
    for (const auto index : sorted) {
        const auto line = anchors[index].line;
        while (hunk_index < entry.line_hunks->size()) {
            const auto& hunk = entry.line_hunks->at(hunk_index);
            auto end = hunk_end(hunk);
            if (!end) {
                return std::unexpected(end.error());
            }
            const auto before_anchor = hunk.old_count == 0 ? hunk.old_line <= line : *end <= line;
            if (!before_anchor) {
                break;
            }
            auto added_result = add_count(added, hunk.new_count);
            if (!added_result) {
                return std::unexpected(added_result.error());
            }
            auto removed_result = add_count(removed, hunk.old_count);
            if (!removed_result) {
                return std::unexpected(removed_result.error());
            }
            ++hunk_index;
        }
        if (hunk_index < entry.line_hunks->size()) {
            const auto& hunk = entry.line_hunks->at(hunk_index);
            auto end = hunk_end(hunk);
            if (!end) {
                return std::unexpected(end.error());
            }
            if (hunk.old_count != 0 && line >= hunk.old_line && line < *end) {
                if (hunk.new_count == 0) {
                    output[index].line = hunk.new_line;
                    output[index].column = 1;
                    output[index].moved = output[index].moved || output[index].line != line;
                    output[index].deleted = true;
                    continue;
                }
                const auto relative = std::min(line - hunk.old_line, hunk.new_count - 1);
                if (relative > std::numeric_limits<std::uint64_t>::max() - hunk.new_line) {
                    return std::unexpected(anchor_error(errc::corrupt, "mapped line overflows"));
                }
                output[index].line = hunk.new_line + relative;
                output[index].moved = output[index].moved || output[index].line != line;
                output[index].approximate = true;
                continue;
            }
        }
        auto mapped = shifted_line(line, added, removed);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        output[index].line = *mapped;
        output[index].moved = output[index].moved || output[index].line != line;
    }
    return {};
}

} // namespace

auto repository_detail::text_hunks(std::span<const std::byte> old_bytes,
                                   std::span<const std::byte> new_bytes)
    -> std::optional<std::vector<line_change>> {
    return line_hunks(old_bytes, new_bytes);
}

runtime::task<std::vector<tree_diff_entry>>
repository::diff(change_id old_change, change_id new_change, tree_diff_options options) {
    auto old_snapshot = co_await at(old_change);
    if (!old_snapshot) {
        co_return std::unexpected(old_snapshot.error());
    }
    auto new_snapshot = co_await at(new_change);
    if (!new_snapshot) {
        co_return std::unexpected(new_snapshot.error());
    }
    auto output = co_await compare(
        *this, std::move(old_snapshot->root), std::move(new_snapshot->root), options);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    auto old_history = co_await log({.from = old_change, .first_parent = true});
    if (!old_history) {
        co_return std::unexpected(old_history.error());
    }
    auto new_history = co_await log({.from = new_change, .first_parent = true});
    if (!new_history) {
        co_return std::unexpected(new_history.error());
    }
    if (!old_history->empty() && !new_history->empty()) {
        apply_conflicts(*output, old_history->front().conflicts, new_history->front().conflicts);
        std::ranges::sort(*output, {}, &tree_diff_entry::path);
    }
    co_return *output;
}

runtime::task<std::vector<tree_diff_entry>>
repository::diff(digest old_tree, digest new_tree, tree_diff_options options) {
    auto old_value = co_await tree(old_tree);
    if (!old_value) {
        co_return std::unexpected(old_value.error());
    }
    auto new_value = co_await tree(new_tree);
    if (!new_value) {
        co_return std::unexpected(new_value.error());
    }
    co_return co_await compare(*this, std::move(*old_value), std::move(*new_value), options);
}

runtime::task<std::vector<tree_diff_entry>>
repository::diff(liquid::tree old_tree, liquid::tree new_tree, tree_diff_options options) {
    co_return co_await compare(*this, std::move(old_tree), std::move(new_tree), options);
}

runtime::task<mapped_anchor> repository::map_anchor(text_anchor anchor, change_id target) {
    const auto anchors = std::array<text_anchor, 1>{std::move(anchor)};
    auto mapped = co_await map_anchors(anchors, target);
    if (!mapped) {
        co_return std::unexpected(mapped.error());
    }
    co_return std::move(mapped->front());
}

runtime::task<std::vector<mapped_anchor>>
repository::map_anchors(std::span<const text_anchor> anchors, change_id target) {
    if (anchors.empty()) {
        co_return std::vector<mapped_anchor>{};
    }
    const auto source = anchors.front().change;
    auto output = std::vector<mapped_anchor>{};
    output.reserve(anchors.size());
    for (const auto& anchor : anchors) {
        if (!same_digest(anchor.change.value, source.value)) {
            co_return std::unexpected(anchor_error(
                errc::invalid_argument, "batched text anchors must share one source change"));
        }
        if (anchor.line == 0 || anchor.column == 0) {
            co_return std::unexpected(anchor_error(
                errc::invalid_argument, "text anchor line and column are one-based", anchor.path));
        }
        output.push_back(
            {.change = target, .path = anchor.path, .line = anchor.line, .column = anchor.column});
    }
    if (same_digest(source.value, target.value)) {
        co_return output;
    }
    auto changes = co_await diff(source, target, {.text = true});
    if (!changes) {
        co_return std::unexpected(changes.error());
    }
    auto anchors_by_path = std::unordered_map<std::string_view, std::vector<std::size_t>>{};
    anchors_by_path.reserve(anchors.size());
    for (auto index = std::size_t{}; index != anchors.size(); ++index) {
        anchors_by_path[anchors[index].path].push_back(index);
    }
    for (const auto& entry : *changes) {
        if (has_change(entry, tree_diff_kind::added) && !entry.old_path && !entry.old_blob) {
            continue;
        }
        const auto& source_path = entry.old_path.value_or(entry.path);
        const auto found = anchors_by_path.find(source_path);
        if (found == anchors_by_path.end()) {
            continue;
        }
        const auto& indices = found->second;
        const auto path_moved = entry.old_path && *entry.old_path != entry.path;
        for (const auto index : indices) {
            if (path_moved) {
                output[index].path = entry.path;
                output[index].moved = true;
            }
            if (has_change(entry, tree_diff_kind::removed) || (entry.old_blob && !entry.new_blob)) {
                output[index].deleted = true;
            }
        }
        if (has_change(entry, tree_diff_kind::removed) || (entry.old_blob && !entry.new_blob) ||
            !has_change(entry, tree_diff_kind::modified)) {
            continue;
        }
        auto mapped = map_hunks(anchors, indices, entry, output);
        if (!mapped) {
            co_return std::unexpected(mapped.error());
        }
    }
    co_return output;
}

} // namespace liquid
