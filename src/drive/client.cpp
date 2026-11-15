// SPDX-License-Identifier: MIT

#include "liquid/drive.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "drive/settings.hpp"
#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "repository/drive_access.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;
constexpr auto remote_metadata_key = "drive.remote";
constexpr auto state_metadata_key = "drive.state.v1";

struct file_value {
    std::string path;
    tree_entry entry;
    digest content;
};

using file_map = std::map<std::string, file_value>;

struct persisted_state {
    drive_tree_state trees;
    std::uint64_t files{};
    std::uint64_t state_bytes{};
    std::uint64_t full_walks{};
    std::vector<sync_ref_outcome> references;
};

struct walk_entry {
    std::string path;
    std::filesystem::path source;
    std::uint32_t mode{0100644};
};

struct walk_result {
    std::vector<walk_entry> entries;
    std::uint64_t state_bytes{};
};

struct reconciliation_result {
    file_map desired;
    std::vector<drive_reconciliation> reports;
    std::vector<conflict> conflicts;
};

auto drive_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto digest_text(const std::optional<digest>& value) -> cbor::value {
    return value ? cbor::value{to_string(*value)} : cbor::value{cbor::null{}};
}

auto field(const cbor::value& value, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto decode_optional_digest(const cbor::value* value) -> result<std::optional<digest>> {
    if (!value || std::holds_alternative<cbor::null>(value->data)) {
        return std::optional<digest>{};
    }
    const auto* text = std::get_if<std::string>(&value->data);
    if (!text) {
        return std::unexpected(drive_error(errc::corrupt, "invalid drive tree digest"));
    }
    auto parsed = parse_digest(*text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::optional<digest>{*parsed};
}

auto encode_state(const persisted_state& state) -> result<std::vector<std::byte>> {
    auto references = cbor::value::array{};
    for (const auto& reference : state.references) {
        references.push_back(cbor::value{cbor::value::map{
            {"name", reference.name},
            {"outcome", static_cast<std::uint64_t>(reference.outcome)},
            {"reason", reference.reason},
        }});
    }
    return cbor::encode(cbor::value{cbor::value::map{
        {"v", std::uint64_t{1}},
        {"remote", digest_text(state.trees.remote)},
        {"local", digest_text(state.trees.local)},
        {"base", digest_text(state.trees.base)},
        {"cursor", state.trees.remote_cursor.token},
        {"interrupted", state.trees.interrupted},
        {"files", state.files},
        {"state_bytes", state.state_bytes},
        {"full_walks", state.full_walks},
        {"references", cbor::value{std::move(references)}},
    }});
}

auto decode_state(std::span<const std::byte> bytes) -> result<persisted_state> {
    auto value = cbor::decode(bytes);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto* version_value = field(*value, "v");
    const auto* cursor_value = field(*value, "cursor");
    const auto* interrupted_value = field(*value, "interrupted");
    const auto* files_value = field(*value, "files");
    const auto* state_bytes_value = field(*value, "state_bytes");
    const auto* full_walks_value = field(*value, "full_walks");
    const auto* references_value = field(*value, "references");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* cursor = cursor_value ? std::get_if<std::string>(&cursor_value->data) : nullptr;
    const auto* interrupted =
        interrupted_value ? std::get_if<bool>(&interrupted_value->data) : nullptr;
    const auto* files = files_value ? std::get_if<std::uint64_t>(&files_value->data) : nullptr;
    const auto* state_bytes =
        state_bytes_value ? std::get_if<std::uint64_t>(&state_bytes_value->data) : nullptr;
    const auto* full_walks =
        full_walks_value ? std::get_if<std::uint64_t>(&full_walks_value->data) : nullptr;
    const auto* references =
        references_value ? std::get_if<cbor::value::array>(&references_value->data) : nullptr;
    if (!version || *version != 1 || !cursor || !interrupted || !files || !state_bytes ||
        !full_walks || !references) {
        return std::unexpected(drive_error(errc::corrupt, "invalid drive state"));
    }
    auto remote = decode_optional_digest(field(*value, "remote"));
    auto local = decode_optional_digest(field(*value, "local"));
    auto base = decode_optional_digest(field(*value, "base"));
    if (!remote || !local || !base) {
        return std::unexpected(drive_error(errc::corrupt, "invalid drive tree state"));
    }
    auto output = persisted_state{.trees = {.remote = *remote,
                                            .local = *local,
                                            .base = *base,
                                            .remote_cursor = {*cursor},
                                            .interrupted = *interrupted},
                                  .files = *files,
                                  .state_bytes = *state_bytes,
                                  .full_walks = *full_walks};
    for (const auto& item : *references) {
        const auto* name_value = field(item, "name");
        const auto* outcome_value = field(item, "outcome");
        const auto* reason_value = field(item, "reason");
        const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
        const auto* outcome =
            outcome_value ? std::get_if<std::uint64_t>(&outcome_value->data) : nullptr;
        const auto* reason = reason_value ? std::get_if<std::string>(&reason_value->data) : nullptr;
        if (!name || !outcome || *outcome < 1 || *outcome > 2 || !reason) {
            return std::unexpected(drive_error(errc::corrupt, "invalid drive ref outcome"));
        }
        output.references.push_back({*name, static_cast<sync_ref_outcome_kind>(*outcome), *reason});
    }
    return output;
}

auto repository_bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto repository_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto excluded_path(std::string_view path) -> bool {
    return path == "LIQUID_REPO" || path == "LIQUID_JOURNAL" || path == ".liquid" ||
           path.starts_with(".liquid/") || path == ".liquid-store" ||
           path.starts_with(".liquid-store/") || path.ends_with(".liquid-drive-tmp");
}

auto within_path(std::string_view path, std::string_view parent) -> bool {
    return parent.empty() || path == parent || path.starts_with(std::string{parent} + "/");
}

auto selected_for_device(const drive_detail::stored_settings& settings, std::string_view path)
    -> bool {
    return drive_detail::effective_state(settings, path) != drive_path_state::not_synced_here;
}

auto file_mode(const std::filesystem::directory_entry& entry, std::error_code& code)
    -> std::uint32_t {
    const auto permissions = entry.status(code).permissions();
    if (code) {
        return 0100644;
    }
    return 0100000U | static_cast<std::uint32_t>(permissions) & 0777U;
}

auto walk_files(const std::filesystem::path& folder,
                std::uint64_t ceiling,
                drive_detail::stored_settings settings) -> result<walk_result> {
    auto output = walk_result{};
    auto code = std::error_code{};
    for (std::filesystem::recursive_directory_iterator
             iterator(folder, std::filesystem::directory_options::skip_permission_denied, code),
         end;
         !code && iterator != end;
         iterator.increment(code)) {
        const auto relative = iterator->path().lexically_relative(folder).generic_string();
        if (excluded_path(relative) || !selected_for_device(settings, relative)) {
            if (iterator->is_directory(code)) {
                iterator.disable_recursion_pending();
            }
            code.clear();
            continue;
        }
        if (!iterator->is_regular_file(code)) {
            code.clear();
            continue;
        }
        const auto mode = file_mode(*iterator, code);
        if (code) {
            return std::unexpected(drive_error(errc::io, code.message(), relative));
        }
        output.state_bytes += sizeof(walk_entry) + relative.size() * 2U + 128U;
        if (output.state_bytes > ceiling) {
            return std::unexpected(drive_error(
                errc::busy, "drive state memory ceiling exceeded", std::to_string(ceiling)));
        }
        output.entries.push_back({relative, iterator->path(), mode});
    }
    if (code) {
        return std::unexpected(drive_error(errc::io, code.message(), folder.string()));
    }
    std::ranges::sort(output.entries, {}, &walk_entry::path);
    return output;
}

runtime::task<void> flatten(repository& source, digest root, std::string prefix, file_map& output) {
    auto value = co_await source.tree(root);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    for (auto entry : value->entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            auto nested = co_await flatten(source, entry.ref, path, output);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            continue;
        }
        if (entry.kind != node_kind::blob) {
            continue;
        }
        auto content = co_await repository_detail::drive_access::content(source, entry);
        if (!content) {
            co_return std::unexpected(content.error());
        }
        output.emplace(path, file_value{path, std::move(entry), *content});
    }
    co_return result<void>{};
}

runtime::task<file_map> flatten(repository& source, std::optional<digest> root) {
    auto output = file_map{};
    if (!root) {
        co_return output;
    }
    auto complete = co_await flatten(source, *root, {}, output);
    if (!complete) {
        co_return std::unexpected(complete.error());
    }
    co_return output;
}

auto node_key(const node_id& node) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(node.size() * 2U);
    for (const auto value : node) {
        const auto byte = std::to_integer<unsigned char>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

auto same_file(const file_value& left, const file_value& right) -> bool {
    return same_digest(left.content, right.content) && left.entry.mode == right.entry.mode;
}

auto same_state(const file_value& left, const file_value& right) -> bool {
    return left.path == right.path && same_file(left, right);
}

auto by_node(const file_map& files) -> std::map<std::string, const file_value*> {
    auto output = std::map<std::string, const file_value*>{};
    for (const auto& [path, value] : files) {
        static_cast<void>(path);
        output.emplace(node_key(value.entry.node), &value);
    }
    return output;
}

auto conflict_path(std::string path, const file_map& occupied) -> std::string {
    auto file = std::filesystem::path{path};
    const auto parent = file.parent_path();
    const auto extension = file.extension().string();
    const auto stem = file.stem().string();
    auto index = std::uint64_t{};
    for (;;) {
        const auto suffix = index == 0 ? std::string{} : " " + std::to_string(index + 1);
        const auto name = stem + " (remote conflict" + suffix + ")" + extension;
        const auto candidate = (parent / name).generic_string();
        if (!occupied.contains(candidate)) {
            return candidate;
        }
        ++index;
    }
}

void add_report(reconciliation_result& output,
                const file_value& value,
                drive_reconciliation_kind kind,
                std::optional<std::string> old_path = std::nullopt,
                std::vector<digest> values = {}) {
    output.reports.push_back(
        {.path = value.path, .old_path = std::move(old_path), .kind = kind, .values = values});
}

void add_conflict(reconciliation_result& output,
                  const file_value* base,
                  const file_value& local,
                  const file_value& remote) {
    auto values = std::vector<digest>{};
    if (base) {
        values.push_back(base->entry.ref);
    }
    values.push_back(local.entry.ref);
    values.push_back(remote.entry.ref);
    output.desired[local.path] = local;
    auto alternate = remote;
    alternate.path = conflict_path(local.path, output.desired);
    alternate.entry.name = std::filesystem::path{alternate.path}.filename().string();
    alternate.entry.node = repository_detail::drive_access::fresh_node();
    output.desired[alternate.path] = alternate;
    output.conflicts.push_back(
        {.base = base ? base->entry.ref : local.entry.ref,
         .left = local.entry.ref,
         .right = remote.entry.ref,
         .region = {.path = local.path,
                    .nodes = conflict_node_range{local.entry.node, remote.entry.node}},
         .kind = conflict_kind::structural,
         .explanation = "drive replicas contain different values at this path"});
    add_report(output,
               local,
               drive_reconciliation_kind::content_conflict,
               std::nullopt,
               std::move(values));
}

void reconcile_single_side(reconciliation_result& output,
                           const file_value& base,
                           const file_value& value,
                           bool is_local) {
    if (same_state(value, base)) {
        add_report(output,
                   value,
                   is_local ? drive_reconciliation_kind::remote_only
                            : drive_reconciliation_kind::local_only);
        return;
    }
    output.desired[value.path] = value;
    auto values = std::vector<digest>{base.entry.ref, value.entry.ref};
    output.conflicts.push_back(
        {.base = base.entry.ref,
         .left = base.entry.ref,
         .right = value.entry.ref,
         .region = {.path = value.path,
                    .nodes = conflict_node_range{base.entry.node, value.entry.node}},
         .kind = conflict_kind::structural,
         .explanation = "one drive replica deleted a path changed by the other"});
    add_report(
        output, value, drive_reconciliation_kind::delete_edit, std::nullopt, std::move(values));
}

void reconcile_changed_pair(reconciliation_result& output,
                            const file_value& base,
                            const file_value& local,
                            const file_value& remote) {
    if (same_file(local, remote) && local.path == remote.path) {
        output.desired[local.path] = local;
        return;
    }
    const auto local_moved = local.path != base.path;
    const auto remote_moved = remote.path != base.path;
    const auto local_edited = !same_file(local, base);
    const auto remote_edited = !same_file(remote, base);
    if (local_moved && !remote_moved && !local_edited && remote_edited) {
        auto combined = remote;
        combined.path = local.path;
        combined.entry.name = local.entry.name;
        output.desired[combined.path] = combined;
        add_report(output, combined, drive_reconciliation_kind::moved, base.path);
        return;
    }
    if (remote_moved && !local_moved && !remote_edited && local_edited) {
        auto combined = local;
        combined.path = remote.path;
        combined.entry.name = remote.entry.name;
        output.desired[combined.path] = combined;
        add_report(output, combined, drive_reconciliation_kind::moved, base.path);
        return;
    }
    add_conflict(output, &base, local, remote);
}

void reconcile_established_node(reconciliation_result& output,
                                const file_value& base,
                                const file_value* local,
                                const file_value* remote) {
    if (!local && !remote) {
        return;
    }
    if (local && !remote) {
        reconcile_single_side(output, base, *local, true);
        return;
    }
    if (!local && remote) {
        reconcile_single_side(output, base, *remote, false);
        return;
    }
    const auto local_changed = !same_state(*local, base);
    const auto remote_changed = !same_state(*remote, base);
    if (!local_changed && !remote_changed) {
        output.desired[base.path] = base;
        return;
    }
    if (local_changed && !remote_changed) {
        output.desired[local->path] = *local;
        add_report(output,
                   *local,
                   local->path == base.path ? drive_reconciliation_kind::local_only
                                            : drive_reconciliation_kind::moved,
                   local->path == base.path ? std::optional<std::string>{}
                                            : std::optional<std::string>{base.path});
        return;
    }
    if (!local_changed && remote_changed) {
        output.desired[remote->path] = *remote;
        add_report(output,
                   *remote,
                   remote->path == base.path ? drive_reconciliation_kind::remote_only
                                             : drive_reconciliation_kind::moved,
                   remote->path == base.path ? std::optional<std::string>{}
                                             : std::optional<std::string>{base.path});
        return;
    }
    reconcile_changed_pair(output, base, *local, *remote);
}

auto reconcile_files(const file_map& base, const file_map& local, const file_map& remote)
    -> reconciliation_result {
    auto output = reconciliation_result{};
    const auto local_nodes = by_node(local);
    const auto remote_nodes = by_node(remote);
    auto consumed_local = std::set<std::string>{};
    auto consumed_remote = std::set<std::string>{};
    for (const auto& [base_path, base_value] : base) {
        static_cast<void>(base_path);
        const auto key = node_key(base_value.entry.node);
        const auto local_found = local_nodes.find(key);
        const auto remote_found = remote_nodes.find(key);
        const auto* local_value = local_found == local_nodes.end() ? nullptr : local_found->second;
        const auto* remote_value =
            remote_found == remote_nodes.end() ? nullptr : remote_found->second;
        if (local_value) {
            consumed_local.insert(local_value->path);
        }
        if (remote_value) {
            consumed_remote.insert(remote_value->path);
        }
        reconcile_established_node(output, base_value, local_value, remote_value);
    }
    for (const auto& [path, local_value] : local) {
        if (consumed_local.contains(path)) {
            continue;
        }
        const auto remote_found = remote.find(path);
        if (remote_found == remote.end() || consumed_remote.contains(path)) {
            output.desired[path] = local_value;
            add_report(output, local_value, drive_reconciliation_kind::local_only);
            continue;
        }
        consumed_remote.insert(path);
        if (same_file(local_value, remote_found->second)) {
            output.desired[path] = local_value;
            continue;
        }
        add_conflict(output, nullptr, local_value, remote_found->second);
    }
    for (const auto& [path, remote_value] : remote) {
        if (consumed_remote.contains(path)) {
            continue;
        }
        output.desired[path] = remote_value;
        add_report(output, remote_value, drive_reconciliation_kind::remote_only);
    }
    return output;
}

auto contains_reference(const file_map& files, const digest& reference) -> bool {
    return std::ranges::any_of(
        files, [&](const auto& item) { return same_digest(item.second.entry.ref, reference); });
}

void retain_unresolved_conflicts(reconciliation_result& output,
                                 std::span<const conflict> existing) {
    for (const auto& value : existing) {
        const auto replaced = std::ranges::any_of(output.conflicts, [&](const auto& current) {
            return current.region.path == value.region.path;
        });
        if (replaced || !output.desired.contains(value.region.path)) {
            continue;
        }
        const auto left_is_base = same_digest(value.left, value.base);
        const auto right_is_base = same_digest(value.right, value.base);
        auto retained = false;
        if (left_is_base) {
            retained = contains_reference(output.desired, value.right);
        } else if (right_is_base) {
            retained = contains_reference(output.desired, value.left);
        } else {
            retained = contains_reference(output.desired, value.left) &&
                       contains_reference(output.desired, value.right);
        }
        if (retained) {
            output.conflicts.push_back(value);
        }
    }
}

auto same_conflicts(std::span<const conflict> left, std::span<const conflict> right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (auto index = std::size_t{}; index < left.size(); ++index) {
        if (left[index].region.path != right[index].region.path ||
            !same_digest(left[index].base, right[index].base) ||
            !same_digest(left[index].left, right[index].left) ||
            !same_digest(left[index].right, right[index].right)) {
            return false;
        }
    }
    return true;
}

auto file_maps_equal(const file_map& left, const file_map& right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (const auto& [path, value] : left) {
        const auto found = right.find(path);
        if (found == right.end() || !same_file(value, found->second) ||
            value.entry.node != found->second.entry.node) {
            return false;
        }
    }
    return true;
}

auto pending_upload_count(const file_map& base, const file_map& current) -> std::uint64_t {
    auto paths = std::set<std::string>{};
    for (const auto& [path, value] : base) {
        static_cast<void>(value);
        paths.insert(path);
    }
    for (const auto& [path, value] : current) {
        static_cast<void>(value);
        paths.insert(path);
    }
    auto count = std::uint64_t{};
    for (const auto& path : paths) {
        const auto left = base.find(path);
        const auto right = current.find(path);
        if (left == base.end() || right == current.end() ||
            !same_state(left->second, right->second)) {
            ++count;
        }
    }
    return count;
}

runtime::task<void> hydrate_file(repository& source, const file_value& value) {
    auto blob = co_await repository_detail::drive_access::load_blob(source, value.entry);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    for (const auto& chunk : blob->chunks) {
        auto bytes = co_await repository_detail::drive_access::objects(source).get(chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
    }
    co_return result<void>{};
}

runtime::task<drive_path_status> inspect_path(repository& source,
                                              const drive_detail::stored_settings& settings,
                                              std::string path,
                                              const file_value* value) {
    auto output = drive_path_status{.path = std::move(path)};
    output.state = drive_detail::effective_state(settings, output.path);
    output.hydrated = settings.hydrated.contains(output.path);
    if (value) {
        auto blob = co_await repository_detail::drive_access::load_blob(source, value->entry);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        output.size = blob->size;
        auto all_local = true;
        for (auto index = std::size_t{}; index < blob->chunks.size(); ++index) {
            auto available =
                co_await repository_detail::drive_access::objects(source).has(blob->chunks[index]);
            if (!available) {
                co_return std::unexpected(available.error());
            }
            if (*available != blob_availability::local &&
                output.state != drive_path_state::not_synced_here) {
                all_local = false;
                output.hydration_debt += blob->chunk_sizes[index];
            }
        }
        output.hydrated = output.hydrated || all_local;
    }
    if (output.state == drive_path_state::not_synced_here) {
        output.reason = "excluded on this device";
    } else if (output.state == drive_path_state::available_offline && output.hydration_debt != 0) {
        output.reason = "waiting for the peer to hydrate pinned content";
    } else if (output.state == drive_path_state::available_offline) {
        output.reason = "pinned and available offline";
    } else if (output.hydrated) {
        output.reason = "hydrated by an earlier read";
    } else {
        output.reason = "online-only; hydrates on first read";
    }
    co_return output;
}

auto path_depth(std::string_view path) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count(path, '/'));
}

runtime::task<void> apply_reconciliation(repository& destination,
                                         const file_map& current,
                                         const reconciliation_result& reconciliation) {
    auto desired_nodes = by_node(reconciliation.desired);
    auto temporary_paths = std::map<std::string, std::string>{};
    auto temporary_index = std::uint64_t{};
    for (const auto& [path, value] : current) {
        const auto found = desired_nodes.find(node_key(value.entry.node));
        if (found == desired_nodes.end() || found->second->path == path) {
            continue;
        }
        auto temporary = std::string{};
        do {
            temporary = ".liquid-drive-move-" + std::to_string(++temporary_index);
        } while (current.contains(temporary) || reconciliation.desired.contains(temporary));
        auto moved = co_await destination.move(path, temporary);
        if (!moved) {
            co_return std::unexpected(moved.error());
        }
        temporary_paths.emplace(node_key(value.entry.node), std::move(temporary));
    }
    auto removals = std::vector<std::string>{};
    for (const auto& [path, value] : current) {
        static_cast<void>(value);
        if (!reconciliation.desired.contains(path) &&
            !std::ranges::any_of(temporary_paths,
                                 [&](const auto& item) { return item.second == path; })) {
            removals.push_back(path);
        }
    }
    std::ranges::sort(removals, [](const std::string& left, const std::string& right) {
        return std::pair{path_depth(left), left} > std::pair{path_depth(right), right};
    });
    for (const auto& path : removals) {
        auto removed = co_await destination.remove(path);
        if (!removed && removed.error().code != errc::not_found) {
            co_return std::unexpected(removed.error());
        }
    }
    for (const auto& [path, value] : reconciliation.desired) {
        const auto temporary = temporary_paths.find(node_key(value.entry.node));
        if (temporary != temporary_paths.end()) {
            auto moved = co_await destination.move(temporary->second, path);
            if (moved) {
                continue;
            }
            if (moved.error().code != errc::not_found) {
                co_return std::unexpected(moved.error());
            }
        }
        const auto existing = current.find(path);
        if (existing != current.end() && existing->second.entry.node == value.entry.node &&
            same_file(existing->second, value)) {
            continue;
        }
        if (existing != current.end() && existing->second.entry.node != value.entry.node) {
            auto removed = co_await destination.remove(path);
            if (!removed && removed.error().code != errc::not_found) {
                co_return std::unexpected(removed.error());
            }
        }
        auto written =
            co_await repository_detail::drive_access::write(destination, path, value.entry);
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    auto conflicts = co_await destination.conflicts();
    if (!conflicts) {
        co_return std::unexpected(conflicts.error());
    }
    if (!same_conflicts(*conflicts, reconciliation.conflicts)) {
        auto replaced = co_await repository_detail::drive_access::replace_conflicts(
            destination, reconciliation.conflicts);
        if (!replaced) {
            co_return std::unexpected(replaced.error());
        }
    }
    co_return result<void>{};
}

auto open_output(const std::filesystem::path& path) -> result<std::shared_ptr<std::ofstream>> {
    auto output = std::make_shared<std::ofstream>(path, std::ios::binary | std::ios::trunc);
    if (!*output) {
        return std::unexpected(
            drive_error(errc::io, "cannot create drive temporary file", path.string()));
    }
    return output;
}

auto append_output(const std::shared_ptr<std::ofstream>& output,
                   std::vector<std::byte> bytes,
                   const std::filesystem::path& path) -> result<void> {
    output->write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!*output) {
        return std::unexpected(
            drive_error(errc::io, "cannot write drive temporary file", path.string()));
    }
    return {};
}

auto close_output(const std::shared_ptr<std::ofstream>& output, const std::filesystem::path& path)
    -> result<void> {
    output->flush();
    output->close();
    if (!*output) {
        return std::unexpected(
            drive_error(errc::io, "cannot flush drive temporary file", path.string()));
    }
    return {};
}

auto publish_temporary(const std::filesystem::path& temporary,
                       const std::filesystem::path& destination,
                       std::uint32_t mode,
                       bool durable) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::permissions(temporary,
                                 static_cast<std::filesystem::perms>(mode & 0777U),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code) {
        return std::unexpected(drive_error(errc::io, code.message(), temporary.string()));
    }
#ifndef _WIN32
    if (durable) {
        const auto descriptor = ::open(temporary.c_str(), O_RDONLY);
        if (descriptor < 0 || ::fsync(descriptor) != 0) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
            return std::unexpected(
                drive_error(errc::io, "cannot fsync drive file", temporary.string()));
        }
        ::close(descriptor);
    }
#else
    static_cast<void>(durable);
#endif
    std::filesystem::rename(temporary, destination, code);
    if (code) {
        std::filesystem::remove(destination, code);
        code.clear();
        std::filesystem::rename(temporary, destination, code);
    }
    if (code) {
        return std::unexpected(drive_error(errc::io, code.message(), destination.string()));
    }
#ifndef _WIN32
    if (durable) {
        const auto parent = destination.parent_path().empty() ? "." : destination.parent_path();
        const auto descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (descriptor < 0 || ::fsync(descriptor) != 0) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
            return std::unexpected(
                drive_error(errc::io, "cannot fsync drive directory", parent.string()));
        }
        ::close(descriptor);
    }
#endif
    return {};
}

runtime::task<void> materialise(repository& source,
                                const file_value& value,
                                const std::filesystem::path& folder,
                                bool durable) {
    const auto destination = folder / value.path;
    auto prepared = co_await runtime::offload([parent = destination.parent_path()] {
        auto code = std::error_code{};
        std::filesystem::create_directories(parent, code);
        if (code) {
            return result<void>{
                std::unexpected(drive_error(errc::io, code.message(), parent.string()))};
        }
        return result<void>{};
    });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    auto temporary = destination;
    temporary += ".liquid-drive-tmp";
    auto opened = co_await runtime::offload([temporary] { return open_output(temporary); });
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (!*opened) {
        co_return std::unexpected(opened->error());
    }
    auto blob = co_await repository_detail::drive_access::load_blob(source, value.entry);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    for (const auto& chunk : blob->chunks) {
        auto bytes = co_await repository_detail::drive_access::objects(source).get(chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto appended = co_await runtime::offload(
            [output = **opened, bytes = std::move(*bytes), temporary]() mutable {
                return append_output(output, std::move(bytes), temporary);
            });
        if (!appended) {
            co_return std::unexpected(appended.error());
        }
        if (!*appended) {
            co_return std::unexpected(appended->error());
        }
    }
    auto closed = co_await runtime::offload(
        [output = **opened, temporary] { return close_output(output, temporary); });
    if (!closed) {
        co_return std::unexpected(closed.error());
    }
    if (!*closed) {
        co_return std::unexpected(closed->error());
    }
    auto published =
        co_await runtime::offload([temporary, destination, mode = value.entry.mode, durable] {
            return publish_temporary(temporary, destination, mode, durable);
        });
    if (!published) {
        co_return std::unexpected(published.error());
    }
    if (!*published) {
        co_return std::unexpected(published->error());
    }
    co_return result<void>{};
}

runtime::task<void> publish_files(repository& source,
                                  const file_map& before,
                                  const file_map& desired,
                                  const std::filesystem::path& folder,
                                  const drive_detail::stored_settings& settings,
                                  bool durable) {
    for (const auto& [path, value] : desired) {
        if (!selected_for_device(settings, path)) {
            auto removed = co_await runtime::offload([target = folder / path] {
                auto code = std::error_code{};
                std::filesystem::remove(target, code);
                if (code) {
                    return result<void>{
                        std::unexpected(drive_error(errc::io, code.message(), target.string()))};
                }
                return result<void>{};
            });
            if (!removed) {
                co_return std::unexpected(removed.error());
            }
            if (!*removed) {
                co_return std::unexpected(removed->error());
            }
            continue;
        }
        const auto found = before.find(path);
        if (found != before.end() && same_file(found->second, value)) {
            continue;
        }
        auto blob = co_await repository_detail::drive_access::load_blob(source, value.entry);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        if (!drive_detail::should_hydrate_fully(settings, path, blob->size)) {
            continue;
        }
        auto published = co_await materialise(source, value, folder, durable);
        if (!published) {
            co_return std::unexpected(published.error());
        }
    }
    for (const auto& [path, value] : before) {
        static_cast<void>(value);
        if (desired.contains(path)) {
            continue;
        }
        auto removed = co_await runtime::offload([target = folder / path] {
            auto code = std::error_code{};
            std::filesystem::remove(target, code);
            if (code) {
                return result<void>{
                    std::unexpected(drive_error(errc::io, code.message(), target.string()))};
            }
            return result<void>{};
        });
        if (!removed) {
            co_return std::unexpected(removed.error());
        }
        if (!*removed) {
            co_return std::unexpected(removed->error());
        }
    }
    co_return result<void>{};
}

auto shadow_identifier(std::string_view remote) -> result<std::filesystem::path> {
    const auto bytes = std::span{reinterpret_cast<const std::byte*>(remote.data()), remote.size()};
    auto id = hash(algorithm::blake3, bytes);
    if (!id) {
        return std::unexpected(id.error());
    }
    return std::filesystem::path{"drive-remote-" + to_string(*id)};
}

} // namespace

struct drive_client::impl {
    std::filesystem::path folder;
    std::string remote_url;
    drive_options options;
    repository local;
    repository remote;
    std::shared_ptr<sync_endpoint> remote_source;
    persisted_state state;

    runtime::task<drive_detail::stored_settings> settings() {
        co_return co_await drive_detail::load_settings(local);
    }

    runtime::task<void> persist() {
        auto encoded = encode_state(state);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        co_return co_await local.set_metadata(state_metadata_key, std::move(*encoded));
    }

    runtime::task<file_map> scan() {
        auto configured = co_await settings();
        if (!configured) {
            co_return std::unexpected(configured.error());
        }
        auto walked = co_await runtime::offload([folder = folder,
                                                 ceiling = options.state_memory_ceiling,
                                                 configured = std::move(*configured)] {
            return walk_files(folder, ceiling, std::move(configured));
        });
        if (!walked) {
            co_return std::unexpected(walked.error());
        }
        if (!*walked) {
            co_return std::unexpected(walked->error());
        }
        auto output = file_map{};
        for (const auto& item : (**walked).entries) {
            auto staged = co_await repository_detail::drive_access::stage(
                local,
                item.source,
                std::filesystem::path{item.path}.filename().string(),
                item.mode);
            if (!staged) {
                co_return std::unexpected(staged.error());
            }
            output.emplace(item.path,
                           file_value{item.path, std::move(staged->entry), staged->content});
        }
        state.files = output.size();
        state.state_bytes = (**walked).state_bytes;
        ++state.full_walks;
        co_return output;
    }

    runtime::task<file_map> capture_local() {
        auto root = co_await repository_detail::drive_access::snapshot(local);
        if (!root) {
            co_return std::unexpected(root.error());
        }
        auto current = co_await flatten(local, *root);
        if (!current) {
            co_return std::unexpected(current.error());
        }
        auto base = co_await flatten(local, state.trees.base);
        if (!base) {
            co_return std::unexpected(base.error());
        }
        auto filesystem = co_await scan();
        if (!filesystem) {
            co_return std::unexpected(filesystem.error());
        }
        auto configured = co_await settings();
        if (!configured) {
            co_return std::unexpected(configured.error());
        }
        const auto base_nodes = by_node(*base);
        const auto current_nodes = by_node(*current);
        for (const auto& [path, value] : *base) {
            if (!current_nodes.contains(node_key(value.entry.node))) {
                filesystem->erase(path);
            }
        }
        for (const auto& [path, value] : *current) {
            const auto previous = base_nodes.find(node_key(value.entry.node));
            if (previous == base_nodes.end() || !same_state(value, *previous->second)) {
                if (previous != base_nodes.end()) {
                    filesystem->erase(previous->second->path);
                }
                filesystem->insert_or_assign(path, value);
                continue;
            }
            if (!selected_for_device(*configured, path)) {
                filesystem->insert_or_assign(path, value);
                continue;
            }
            if (filesystem->contains(path)) {
                continue;
            }
            const auto state = drive_detail::effective_state(*configured, path);
            if (state == drive_path_state::available_offline ||
                configured->hydrated.contains(path)) {
                filesystem->insert_or_assign(path, value);
                continue;
            }
            auto blob = co_await repository_detail::drive_access::load_blob(local, value.entry);
            if (!blob) {
                co_return std::unexpected(blob.error());
            }
            if (drive_detail::should_hydrate_fully(*configured, path, blob->size)) {
                continue;
            }
            auto remote_chunks = false;
            for (const auto& chunk : blob->chunks) {
                auto available =
                    co_await repository_detail::drive_access::objects(local).has(chunk);
                if (!available) {
                    co_return std::unexpected(available.error());
                }
                remote_chunks = remote_chunks || *available == blob_availability::remote;
            }
            if (remote_chunks) {
                filesystem->insert_or_assign(path, value);
            }
        }
        auto removed_by_content = std::map<std::string, std::vector<const file_value*>>{};
        for (const auto& [path, value] : *current) {
            if (!filesystem->contains(path)) {
                removed_by_content[to_string(value.content)].push_back(&value);
            }
        }
        for (auto& [path, value] : *filesystem) {
            const auto existing = current->find(path);
            if (existing != current->end()) {
                value.entry.node = existing->second.entry.node;
                continue;
            }
            auto candidates = removed_by_content.find(to_string(value.content));
            if (candidates == removed_by_content.end() || candidates->second.empty()) {
                continue;
            }
            value.entry.node = candidates->second.front()->entry.node;
            candidates->second.erase(candidates->second.begin());
        }
        auto desired = reconciliation_result{.desired = *filesystem};
        auto conflicts = co_await local.conflicts();
        if (!conflicts) {
            co_return std::unexpected(conflicts.error());
        }
        retain_unresolved_conflicts(desired, *conflicts);
        auto applied = co_await apply_reconciliation(local, *current, desired);
        if (!applied) {
            co_return std::unexpected(applied.error());
        }
        auto captured_root = co_await repository_detail::drive_access::snapshot(local);
        if (!captured_root) {
            co_return std::unexpected(captured_root.error());
        }
        if (!file_maps_equal(*current, *filesystem)) {
            auto cut = co_await local.cut("drive filesystem reconciliation");
            if (!cut) {
                co_return std::unexpected(cut.error());
            }
            captured_root = co_await repository_detail::drive_access::snapshot(local);
            if (!captured_root) {
                co_return std::unexpected(captured_root.error());
            }
        }
        state.trees.local = *captured_root;
        co_return *filesystem;
    }
};

drive_client::drive_client() noexcept = default;
drive_client::~drive_client() = default;
drive_client::drive_client(drive_client&&) noexcept = default;
drive_client& drive_client::operator=(drive_client&&) noexcept = default;
drive_client::drive_client(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

runtime::task<drive_client> drive_client::open_impl(std::filesystem::path folder,
                                                    std::optional<std::string> requested_remote,
                                                    drive_options options) {
    auto prepared = co_await runtime::offload([folder] {
        auto code = std::error_code{};
        std::filesystem::create_directories(folder, code);
        if (code) {
            return result<std::filesystem::path>{
                std::unexpected(drive_error(errc::io, code.message(), folder.string()))};
        }
        return result<std::filesystem::path>{
            std::filesystem::weakly_canonical(std::filesystem::absolute(folder))};
    });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    folder = std::move(**prepared);
    auto local = std::filesystem::exists(folder / "LIQUID_REPO")
                     ? co_await repository::open(folder)
                     : co_await repository::create(folder);
    if (!local) {
        co_return std::unexpected(local.error());
    }
    auto configured = co_await local->metadata(remote_metadata_key);
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto remote_url = configured->has_value() ? repository_text(**configured) : std::string{};
    if (requested_remote) {
        if (!remote_url.empty() && remote_url != *requested_remote) {
            co_return std::unexpected(drive_error(
                errc::conflict, "folder is configured for a different drive remote", remote_url));
        }
        remote_url = std::move(*requested_remote);
    }
    if (remote_url.empty()) {
        co_return std::unexpected(
            drive_error(errc::not_found, "folder has no configured drive remote", folder.string()));
    }
    if (!configured->has_value()) {
        auto saved =
            co_await local->set_metadata(remote_metadata_key, repository_bytes(remote_url));
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
    }
    auto state = persisted_state{};
    auto encoded_state = co_await local->metadata(state_metadata_key);
    if (!encoded_state) {
        co_return std::unexpected(encoded_state.error());
    }
    if (*encoded_state) {
        auto decoded = decode_state(**encoded_state);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        state = std::move(*decoded);
    }
    auto identifier = shadow_identifier(remote_url);
    if (!identifier) {
        co_return std::unexpected(identifier.error());
    }
    auto& objects = repository_detail::drive_access::objects(*local);
    auto repository_options = liquid::repository_options{.store = &objects};
    auto remote = co_await repository::open(*identifier, repository_options);
    if (!remote && (remote.error().code == errc::not_found || remote.error().code == errc::io)) {
        auto created = co_await repository::create(*identifier, repository_options);
        if (created) {
            remote = std::move(created);
        }
    }
    if (!remote) {
        co_return std::unexpected(remote.error());
    }
    auto implementation = std::make_unique<drive_client::impl>();
    implementation->folder = std::move(folder);
    implementation->remote_url = std::move(remote_url);
    implementation->options = std::move(options);
    implementation->local = std::move(*local);
    implementation->remote = std::move(*remote);
    implementation->state = std::move(state);
    auto source_options =
        sync_endpoint_options{.bearer_token = implementation->options.bearer_token,
                              .batch_items = implementation->options.batch_items};
    implementation->remote_source =
        std::make_shared<sync_endpoint>(implementation->remote_url, std::move(source_options));
    auto registered =
        co_await repository_detail::drive_access::objects(implementation->local)
            .register_remote_source(implementation->remote_url, implementation->remote_source);
    if (!registered) {
        co_return std::unexpected(registered.error());
    }
    co_return drive_client{std::move(implementation)};
}

runtime::task<drive_client>
drive_client::open(std::filesystem::path folder, std::string remote, drive_options options) {
    co_return co_await open_impl(std::move(folder), std::move(remote), std::move(options));
}

runtime::task<drive_client> drive_client::open(std::filesystem::path folder,
                                               drive_options options) {
    co_return co_await open_impl(std::move(folder), std::nullopt, std::move(options));
}

runtime::task<drive_status> drive_client::status() {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    auto conflicts = co_await impl_->local.conflicts();
    if (!conflicts) {
        co_return std::unexpected(conflicts.error());
    }
    auto configured = co_await impl_->settings();
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto current_root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!current_root) {
        co_return std::unexpected(current_root.error());
    }
    auto current = co_await flatten(impl_->local, *current_root);
    auto base = co_await flatten(impl_->local, impl_->state.trees.base);
    if (!current || !base) {
        co_return std::unexpected(!current ? current.error() : base.error());
    }
    auto paths = std::vector<drive_path_status>{};
    auto hydration_debt = std::uint64_t{};
    for (const auto& [path, value] : *current) {
        auto inspected = co_await inspect_path(impl_->local, *configured, path, &value);
        if (!inspected) {
            co_return std::unexpected(inspected.error());
        }
        hydration_debt += inspected->hydration_debt;
        paths.push_back(std::move(*inspected));
    }
    for (const auto& [path, state] : configured->states) {
        static_cast<void>(state);
        if (current->contains(path)) {
            continue;
        }
        auto inspected = co_await inspect_path(impl_->local, *configured, path, nullptr);
        if (!inspected) {
            co_return std::unexpected(inspected.error());
        }
        paths.push_back(std::move(*inspected));
    }
    std::ranges::sort(paths, {}, &drive_path_status::path);
    co_return drive_status{.folder = impl_->folder,
                           .remote = impl_->remote_url,
                           .trees = impl_->state.trees,
                           .files = impl_->state.files,
                           .state_bytes = impl_->state.state_bytes,
                           .state_memory_ceiling = impl_->options.state_memory_ceiling,
                           .full_walks = impl_->state.full_walks,
                           .settings = configured->policy,
                           .pending_uploads = pending_upload_count(*base, *current),
                           .hydration_debt = hydration_debt,
                           .offline_reason = configured->offline_reason,
                           .paths = std::move(paths),
                           .conflicts = std::move(*conflicts),
                           .references = impl_->state.references};
}

runtime::task<drive_path_status> drive_client::path_status(std::string path) {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    auto normalized = drive_detail::normalize_path(std::move(path));
    if (!normalized) {
        co_return std::unexpected(normalized.error());
    }
    auto configured = co_await impl_->settings();
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto files = co_await flatten(impl_->local, *root);
    if (!files) {
        co_return std::unexpected(files.error());
    }
    const auto found = files->find(*normalized);
    if (found != files->end()) {
        co_return co_await inspect_path(impl_->local, *configured, *normalized, &found->second);
    }
    const auto directory = std::ranges::any_of(*files, [&](const auto& item) {
        return within_path(item.first, *normalized) && item.first != *normalized;
    });
    if (!directory && !normalized->empty()) {
        co_return std::unexpected(
            drive_error(errc::not_found, "drive path does not exist", *normalized));
    }
    co_return co_await inspect_path(impl_->local, *configured, *normalized, nullptr);
}

runtime::task<drive_settings> drive_client::settings() {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    auto configured = co_await impl_->settings();
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    co_return configured->policy;
}

runtime::task<void> drive_client::configure(drive_settings settings) {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    if (settings.hydration_size_threshold == 0) {
        co_return std::unexpected(
            drive_error(errc::invalid_argument, "hydration size threshold must be positive"));
    }
    auto configured = co_await impl_->settings();
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    configured->policy = settings;
    co_return co_await drive_detail::save_settings(impl_->local, *configured);
}

runtime::task<void> drive_client::set_state(std::string path, drive_path_state state) {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    auto normalized = drive_detail::normalize_path(std::move(path));
    if (!normalized) {
        co_return std::unexpected(normalized.error());
    }
    auto saved = co_await drive_detail::set_path_state(impl_->local, *normalized, state);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    if (state != drive_path_state::available_offline) {
        co_return result<void>{};
    }
    auto root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto files = co_await flatten(impl_->local, *root);
    if (!files) {
        co_return std::unexpected(files.error());
    }
    auto matched = false;
    for (const auto& [candidate, value] : *files) {
        if (candidate != *normalized &&
            (!saved->policy.pin_folders || !within_path(candidate, *normalized))) {
            continue;
        }
        matched = true;
        auto hydrated = co_await hydrate_file(impl_->local, value);
        if (!hydrated) {
            const auto reason = "pinned content is unavailable: " + hydrated.error().message;
            auto recorded = co_await drive_detail::record_offline_reason(impl_->local, reason);
            if (!recorded) {
                co_return std::unexpected(recorded.error());
            }
            co_return std::unexpected(hydrated.error());
        }
        auto recorded = co_await drive_detail::record_hydrated(impl_->local, candidate);
        if (!recorded) {
            co_return std::unexpected(recorded.error());
        }
    }
    if (!matched && !normalized->empty()) {
        co_return std::unexpected(
            drive_error(errc::not_found, "drive path does not exist", *normalized));
    }
    co_return result<void>{};
}

runtime::task<void> drive_client::pin(std::string path) {
    co_return co_await set_state(std::move(path), drive_path_state::available_offline);
}

runtime::task<void> drive_client::unpin(std::string path) {
    co_return co_await set_state(std::move(path), drive_path_state::online_only);
}

runtime::task<drive_sync_report> drive_client::sync() {
    if (!impl_) {
        co_return std::unexpected(drive_error(errc::invalid_argument, "drive client is empty"));
    }
    auto reports = std::vector<drive_reconciliation>{};
    if (impl_->state.trees.interrupted) {
        reports.push_back({.kind = drive_reconciliation_kind::interrupted_transfer});
    }
    const auto bootstrap = !impl_->state.trees.local && !impl_->state.trees.base;
    auto local_files = bootstrap ? co_await impl_->scan() : co_await impl_->capture_local();
    if (!local_files) {
        co_return std::unexpected(local_files.error());
    }
    impl_->state.trees.interrupted = true;
    auto persisted = co_await impl_->persist();
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    auto received = sync_transfer_report{};
    if (bootstrap) {
        auto bootstrap_options = sync_endpoint_options{.bearer_token = impl_->options.bearer_token,
                                                       .batch_items = impl_->options.batch_items};
        auto bootstrap_endpoint = sync_endpoint{impl_->remote_url, std::move(bootstrap_options)};
        auto adopted = co_await bootstrap_endpoint.pull_metadata(impl_->local);
        if (!adopted) {
            co_return std::unexpected(adopted.error());
        }
        received.objects += adopted->received.objects;
        received.bytes += adopted->received.bytes;
    }
    auto pull_options = sync_endpoint_options{.bearer_token = impl_->options.bearer_token,
                                              .cursor = impl_->state.trees.remote_cursor,
                                              .batch_items = impl_->options.batch_items};
    auto pull_endpoint = sync_endpoint{impl_->remote_url, std::move(pull_options)};
    auto pulled = co_await pull_endpoint.pull_metadata(impl_->remote);
    if (!pulled) {
        co_return std::unexpected(pulled.error());
    }
    received.objects += pulled->received.objects;
    received.bytes += pulled->received.bytes;
    impl_->state.trees.remote_cursor = pulled->cursor;
    auto remote_root = co_await repository_detail::drive_access::snapshot(impl_->remote);
    if (!remote_root) {
        co_return std::unexpected(remote_root.error());
    }
    impl_->state.trees.remote = *remote_root;
    auto remote_head_changed = false;
    if (!bootstrap) {
        auto observed =
            co_await repository_detail::drive_access::observe(impl_->local, impl_->remote);
        if (!observed) {
            co_return std::unexpected(observed.error());
        }
        remote_head_changed = *observed;
    }
    auto observed_root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!observed_root) {
        co_return std::unexpected(observed_root.error());
    }
    impl_->state.trees.local = *observed_root;
    auto base_files = co_await flatten(impl_->local, impl_->state.trees.base);
    auto repository_files = co_await flatten(impl_->local, impl_->state.trees.local);
    auto remote_files = co_await flatten(impl_->remote, impl_->state.trees.remote);
    if (!base_files || !repository_files || !remote_files) {
        co_return std::unexpected(!base_files         ? base_files.error()
                                  : !repository_files ? repository_files.error()
                                                      : remote_files.error());
    }
    auto reconciliation = reconcile_files(*base_files, *local_files, *remote_files);
    auto existing_conflicts = co_await impl_->local.conflicts();
    if (!existing_conflicts) {
        co_return std::unexpected(existing_conflicts.error());
    }
    retain_unresolved_conflicts(reconciliation, *existing_conflicts);
    reports.insert(reports.end(),
                   std::make_move_iterator(reconciliation.reports.begin()),
                   std::make_move_iterator(reconciliation.reports.end()));
    auto applied = co_await apply_reconciliation(impl_->local, *repository_files, reconciliation);
    if (!applied) {
        co_return std::unexpected(applied.error());
    }
    auto configured = co_await impl_->settings();
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto published = co_await publish_files(impl_->local,
                                            *local_files,
                                            reconciliation.desired,
                                            impl_->folder,
                                            *configured,
                                            impl_->options.durable);
    if (!published) {
        co_return std::unexpected(published.error());
    }
    auto merged_root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!merged_root) {
        co_return std::unexpected(merged_root.error());
    }
    if (!file_maps_equal(*repository_files, reconciliation.desired) ||
        !reconciliation.conflicts.empty() || remote_head_changed) {
        auto cut = co_await repository_detail::drive_access::cut(
            impl_->local, impl_->remote, "drive three-tree reconciliation");
        if (!cut) {
            co_return std::unexpected(cut.error());
        }
        merged_root = co_await repository_detail::drive_access::snapshot(impl_->local);
        if (!merged_root) {
            co_return std::unexpected(merged_root.error());
        }
    }
    impl_->state.trees.local = *merged_root;
    auto push_options = sync_endpoint_options{.bearer_token = impl_->options.bearer_token,
                                              .batch_items = impl_->options.batch_items};
    auto push_endpoint = sync_endpoint{impl_->remote_url, std::move(push_options)};
    auto pushed = co_await push_endpoint.push(impl_->local);
    if (!pushed) {
        co_return std::unexpected(pushed.error());
    }
    impl_->state.references = pushed->references;
    const auto refused = std::ranges::any_of(pushed->references, [](const auto& reference) {
        return reference.outcome == sync_ref_outcome_kind::refused;
    });
    auto final_root = co_await repository_detail::drive_access::snapshot(impl_->local);
    if (!final_root) {
        co_return std::unexpected(final_root.error());
    }
    impl_->state.trees.local = *final_root;
    impl_->state.files = reconciliation.desired.size();
    if (!refused) {
        impl_->state.trees.base = *final_root;
        impl_->state.trees.remote = *final_root;
    }
    impl_->state.trees.interrupted = false;
    persisted = co_await impl_->persist();
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    auto current_status = co_await status();
    if (!current_status) {
        co_return std::unexpected(current_status.error());
    }
    co_return drive_sync_report{.status = std::move(*current_status),
                                .paths = std::move(reports),
                                .sent = pushed->sent,
                                .received = {received.objects + pushed->received.objects,
                                             received.bytes + pushed->received.bytes}};
}

} // namespace liquid
