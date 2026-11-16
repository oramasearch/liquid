// SPDX-License-Identifier: MIT

#include "liquid/repository.hpp"
#include "repository/ref_name.hpp"
#include "liquid/store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "build/budgets.hpp"
#include "build/cache.hpp"
#include "build/policy.hpp"
#include "build/repository_access.hpp"
#include "crdt/document.hpp"
#include "data/repository_access.hpp"
#include "drive/source.hpp"
#include "edit_codec.hpp"
#include "encoding/cbor.hpp"
#include "encryption/leaf.hpp"
#include "flow_control.hpp"
#include "formats/dotenv.hpp"
#include "formats/json.hpp"
#include "identity/taap.hpp"
#include "objects/entry_name.hpp"
#include "objects/records.hpp"
#include "repository/diff.hpp"
#include "repository/drive_access.hpp"
#include "repository/git_access.hpp"
#include "repository/local_state.hpp"
#include "repository/merge.hpp"
#include "repository/store_access.hpp"
#include "repository/sync_access.hpp"
#include "repository/testing.hpp"
#include "repository/transfer.hpp"
#include "search/repository_access.hpp"
#include "store/backend.hpp"
#include "store/holder.hpp"
#include "store/leases.hpp"
#include "store/pack.hpp"
#include "store/references.hpp"
#include "vfs/repository_access.hpp"

#if defined(LIQUID_HAS_BUILTIN_FRONTEND)
extern "C" const liquid_frontend_module_v1* liquid_builtin_tree_sitter_module_v1(void);
#endif

namespace liquid {
namespace {
namespace cbor = encoding::cbor;
enum class entry_kind : std::uint8_t { file, directory, nested_repository };
struct working_entry {
    entry_kind kind{entry_kind::file};
    node_id node{};
    std::uint32_t mode{0100644};
    digest object{};
    std::map<std::string, working_entry> entries;
    std::optional<repo_ref> nested;
};
struct stored_entry_effect {
    std::string path;
    std::optional<working_entry> before;
    std::optional<working_entry> after;
};
struct stored_ref_effect {
    std::string name;
    std::optional<change_id> before;
    std::optional<change_id> after;
};
struct stored_lock_effect {
    std::string path;
    bool before{};
    bool after{};
};
struct stored_operation {
    op_id id{};
    std::string kind;
    std::string from;
    std::string to;
    node_id node{};
    std::optional<working_entry> prior;
    std::vector<stored_entry_effect> entries;
    std::vector<stored_ref_effect> refs;
    std::vector<stored_lock_effect> locks;
    std::optional<change_id> head_before;
    std::optional<change_id> head_after;
    session_id session;
    actor by;
    std::chrono::sys_seconds at;
    bool agent{};
    std::map<std::string, std::string> metadata;
};
struct persisted_drip {
    actor author;
    std::vector<std::byte> body;
    std::vector<std::byte> signature;
};
struct repository_document {
    node_id repository{};
    working_entry root{entry_kind::directory, {}, 0040755, {}, {}};
    std::map<std::string, change_id> refs;
    std::map<std::string, std::vector<std::byte>> metadata;
    std::map<node_id, digest> dirs;
    std::optional<change_id> head;
    std::vector<stored_operation> operations;
    std::vector<exclusive_lock> locks;
    std::vector<semantic_lease> leases;
    std::vector<epoch> epochs;
    std::vector<conflict_reference> conflict_values;
    std::vector<conflict_resolution> pending_resolutions;
    std::vector<persisted_drip> drips;
    std::vector<encryption_path_rule> encryption_rules;
    bool cache_publish{};
    std::uint32_t verification_rate{20};
    agents_config agents;
    hot_update_config hot_update;
    deployment_config deploy;
    policy_config policy;
    retention_config retention{};
    flow_config flow{};
    std::optional<remote_config> remote;
    data_config data;
    lock_config lock_rules;
    std::vector<repository_detail::stored_data_branch> data_branches;
    std::uint64_t pending_drips{};
    bool root_is_paged{};
    std::map<std::string, working_entry> nested_entries;
};

constexpr auto journal_pointer_prefix = std::string_view{"liquid-journal-v2 "};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

void collect_format_entry_bytes(const format_tree& tree,
                                std::vector<std::vector<std::byte>>& output) {
    if (tree.tier == format_tier::chunk_only) {
        return;
    }
    for (const auto& entry : tree.entries) {
        if (entry.kind != format_entry_kind::directory) {
            output.push_back(entry.bytes);
        }
        if (entry.nested) {
            collect_format_entry_bytes(*entry.nested, output);
        }
    }
}

auto unpack_format_entries(std::span<const std::byte> contents, std::string path)
    -> result<std::vector<std::vector<std::byte>>> {
    auto tree = format_registry::global().unpack(contents, path);
    if (!tree) {
        if (tree.error().code == errc::not_found) {
            return std::vector<std::vector<std::byte>>{};
        }
        return std::unexpected(tree.error());
    }
    auto output = std::vector<std::vector<std::byte>>{};
    collect_format_entry_bytes(*tree, output);
    return output;
}

auto store_format_entries(store& objects, std::span<const std::byte> contents, std::string path)
    -> runtime::task<std::vector<digest>> {
    auto unpacked = co_await runtime::offload(
        [contents, path = std::move(path)] { return unpack_format_entries(contents, path); });
    if (!unpacked) {
        co_return std::unexpected(unpacked.error());
    }
    if (!*unpacked) {
        co_return std::unexpected(unpacked->error());
    }
    auto output = std::vector<digest>{};
    output.reserve((**unpacked).size());
    for (auto& entry : **unpacked) {
        auto stored = co_await objects.put(std::move(entry));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (std::ranges::none_of(output, [&](const digest& value) {
                return value.alg == stored->alg && value.bytes == stored->bytes;
            })) {
            output.push_back(*stored);
        }
    }
    co_return output;
}

auto format_file_candidate(std::string_view repository_path, const std::filesystem::path& source)
    -> bool {
    auto& registry = format_registry::global();
    if (!registry.find_by_extension(repository_path).empty()) {
        return true;
    }
    auto input = std::ifstream{source, std::ios::binary};
    if (!input) {
        return false;
    }
    auto prefix = std::array<std::byte, 512>{};
    input.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    const auto size = static_cast<std::size_t>(input.gcount());
    return !registry.find_by_magic(std::span{prefix}.first(size)).empty();
}
auto bytes(std::span<const std::byte> value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}
auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
    return cbor::value{cbor::value::map{values}};
}
auto digest_value(const digest& value) -> cbor::value {
    return map({{"alg", cbor::value{static_cast<std::uint64_t>(value.alg)}},
                {"bytes", bytes(value.bytes)}});
}
auto actor_value(const actor& value) -> cbor::value {
    return map({{"taap_id", cbor::value{value.taap_id}},
                {"algorithm", cbor::value{value.algorithm}},
                {"fingerprint", cbor::value{value.fingerprint}},
                {"public_key", bytes(value.public_key)},
                {"key_fingerprint", cbor::value{value.key_fingerprint}}});
}
auto position_value(const frontend_source_position& value) -> cbor::value {
    return map({{"byte", cbor::value{value.byte}},
                {"line", cbor::value{value.line}},
                {"column", cbor::value{value.column}}});
}
auto range_value(const frontend_source_range& value) -> cbor::value {
    return map({{"start", position_value(value.start)}, {"end", position_value(value.end)}});
}
auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields)
        return nullptr;
    for (const auto& [name, item] : *fields)
        if (const auto* text = std::get_if<std::string>(&name.data); text && *text == key)
            return &item;
    return nullptr;
}
auto decode_digest(const cbor::value& value) -> result<digest> {
    const auto* alg = find(value, "alg");
    const auto* raw = find(value, "bytes");
    const auto* number = alg ? std::get_if<std::uint64_t>(&alg->data) : nullptr;
    const auto* data = raw ? std::get_if<cbor::value::bytes>(&raw->data) : nullptr;
    if (!number || !data || data->size() != 32 || *number < 1 || *number > 3)
        return std::unexpected(failure(errc::corrupt, "invalid digest", "repository document"));
    digest output{static_cast<algorithm>(*number), {}};
    std::ranges::copy(*data, output.bytes.begin());
    return output;
}
auto decode_actor(const cbor::value& value) -> result<actor> {
    const auto* taap_id = find(value, "taap_id");
    const auto* algorithm = find(value, "algorithm");
    const auto* fingerprint = find(value, "fingerprint");
    const auto* public_key = find(value, "public_key");
    const auto* key_fingerprint = find(value, "key_fingerprint");
    const auto* taap_id_text = taap_id ? std::get_if<std::string>(&taap_id->data) : nullptr;
    const auto* algorithm_text = algorithm ? std::get_if<std::string>(&algorithm->data) : nullptr;
    const auto* fingerprint_text =
        fingerprint ? std::get_if<std::string>(&fingerprint->data) : nullptr;
    const auto* public_key_bytes =
        public_key ? std::get_if<cbor::value::bytes>(&public_key->data) : nullptr;
    const auto* key_fingerprint_text =
        key_fingerprint ? std::get_if<std::string>(&key_fingerprint->data) : nullptr;
    if (!taap_id_text || !algorithm_text || !fingerprint_text || !public_key_bytes ||
        !key_fingerprint_text) {
        return std::unexpected(
            failure(errc::corrupt, "invalid operation actor", "repository document"));
    }
    return actor{*taap_id_text,
                 *algorithm_text,
                 *fingerprint_text,
                 *public_key_bytes,
                 *key_fingerprint_text};
}

auto decode_seconds(const cbor::value& value) -> std::optional<std::int64_t> {
    if (const auto* signed_value = std::get_if<std::int64_t>(&value.data)) {
        return *signed_value;
    }
    const auto* unsigned_value = std::get_if<std::uint64_t>(&value.data);
    if (unsigned_value &&
        *unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*unsigned_value);
    }
    return std::nullopt;
}

auto lock_value(const exclusive_lock& value) -> cbor::value {
    return map({{"path", cbor::value{value.path}},
                {"holder", actor_value(value.holder)},
                {"since", cbor::value{value.since.time_since_epoch().count()}},
                {"reason", cbor::value{value.reason}},
                {"authority",
                 cbor::value{value.authority == lock_authority::lake ? "lake" : "advisory"}}});
}

auto decode_lock(const cbor::value& value) -> result<exclusive_lock> {
    const auto* path_value = find(value, "path");
    const auto* holder_value = find(value, "holder");
    const auto* since_value = find(value, "since");
    const auto* reason_value = find(value, "reason");
    const auto* authority_value = find(value, "authority");
    const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
    const auto* reason = reason_value ? std::get_if<std::string>(&reason_value->data) : nullptr;
    const auto* authority =
        authority_value ? std::get_if<std::string>(&authority_value->data) : nullptr;
    auto holder =
        holder_value
            ? decode_actor(*holder_value)
            : result<actor>{std::unexpected(failure(errc::corrupt, "lock holder is missing"))};
    const auto seconds = since_value ? decode_seconds(*since_value) : std::nullopt;
    if (!path || path->empty() || !holder || !seconds || !reason || reason->empty() || !authority ||
        (*authority != "advisory" && *authority != "lake")) {
        return std::unexpected(failure(errc::corrupt, "invalid exclusive lock"));
    }
    return exclusive_lock{.path = *path,
                          .holder = std::move(*holder),
                          .since = std::chrono::sys_seconds{std::chrono::seconds{*seconds}},
                          .reason = *reason,
                          .authority = *authority == "lake" ? lock_authority::lake
                                                            : lock_authority::advisory};
}

auto lock_config_value(const lock_config& value) -> cbor::value {
    auto rules = cbor::value::array{};
    for (const auto& rule : value.rules) {
        auto paths = cbor::value::array{};
        for (const auto& path : rule.paths) {
            paths.push_back(cbor::value{path});
        }
        rules.push_back(
            map({{"name", cbor::value{rule.name}}, {"paths", cbor::value{std::move(paths)}}}));
    }
    return map({{"rules", cbor::value{std::move(rules)}}});
}

auto decode_lock_config(const cbor::value& value) -> result<lock_config> {
    const auto* rules_value = find(value, "rules");
    const auto* rules = rules_value ? std::get_if<cbor::value::array>(&rules_value->data) : nullptr;
    if (!rules) {
        return std::unexpected(failure(errc::corrupt, "invalid lock config"));
    }
    auto output = lock_config{};
    for (const auto& item : *rules) {
        const auto* name_value = find(item, "name");
        const auto* paths_value = find(item, "paths");
        const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
        const auto* paths =
            paths_value ? std::get_if<cbor::value::array>(&paths_value->data) : nullptr;
        if (!name || name->empty() || !paths || paths->empty()) {
            return std::unexpected(failure(errc::corrupt, "invalid lock rule"));
        }
        auto rule = lock_rule{.name = *name};
        for (const auto& path_value : *paths) {
            const auto* path = std::get_if<std::string>(&path_value.data);
            if (!path || path->empty()) {
                return std::unexpected(failure(errc::corrupt, "invalid lock rule path"));
            }
            rule.paths.push_back(*path);
        }
        output.rules.push_back(std::move(rule));
    }
    return output;
}
auto decode_position(const cbor::value& value) -> result<frontend_source_position> {
    const auto* byte_value = find(value, "byte");
    const auto* line_value = find(value, "line");
    const auto* column_value = find(value, "column");
    const auto* byte = byte_value ? std::get_if<std::uint64_t>(&byte_value->data) : nullptr;
    const auto* line = line_value ? std::get_if<std::uint64_t>(&line_value->data) : nullptr;
    const auto* column = column_value ? std::get_if<std::uint64_t>(&column_value->data) : nullptr;
    if (!byte || !line || !column) {
        return std::unexpected(
            failure(errc::corrupt, "invalid source position", "repository document"));
    }
    return frontend_source_position{*byte, *line, *column};
}
auto decode_range(const cbor::value& value) -> result<frontend_source_range> {
    const auto* start_value = find(value, "start");
    const auto* end_value = find(value, "end");
    if (!start_value || !end_value) {
        return std::unexpected(
            failure(errc::corrupt, "invalid source range", "repository document"));
    }
    auto start = decode_position(*start_value);
    auto end = decode_position(*end_value);
    if (!start || !end || start->byte > end->byte) {
        return std::unexpected(
            !start ? start.error()
            : !end ? end.error()
                   : failure(errc::corrupt, "source range is reversed", "repository document"));
    }
    return frontend_source_range{*start, *end};
}
auto encode_presence_body(const liquid::presence& value) -> result<std::vector<std::byte>> {
    return cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}},
             {"actor", actor_value(value.actor)},
             {"session", bytes(value.session.bytes)},
             {"path", cbor::value{value.path}},
             {"range", value.range ? range_value(*value.range) : cbor::value{cbor::null{}}},
             {"activity",
              cbor::value{value.activity == presence_activity::editing   ? "editing"
                          : value.activity == presence_activity::reading ? "reading"
                                                                         : "idle"}},
             {"at", cbor::value{value.at.time_since_epoch().count()}}}));
}
auto decode_presence_body(std::span<const std::byte> input) -> result<liquid::presence> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = find(*decoded, "v");
    const auto* actor_encoded = find(*decoded, "actor");
    const auto* session_value = find(*decoded, "session");
    const auto* path_value = find(*decoded, "path");
    const auto* range_encoded = find(*decoded, "range");
    const auto* activity_value = find(*decoded, "activity");
    const auto* at_value = find(*decoded, "at");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* session =
        session_value ? std::get_if<cbor::value::bytes>(&session_value->data) : nullptr;
    const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
    const auto* activity =
        activity_value ? std::get_if<std::string>(&activity_value->data) : nullptr;
    auto seconds = std::optional<std::int64_t>{};
    if (at_value) {
        if (const auto* signed_value = std::get_if<std::int64_t>(&at_value->data)) {
            seconds = *signed_value;
        } else if (const auto* unsigned_value = std::get_if<std::uint64_t>(&at_value->data);
                   unsigned_value &&
                   *unsigned_value <=
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            seconds = static_cast<std::int64_t>(*unsigned_value);
        }
    }
    if (!version || *version != 1 || !actor_encoded || !session ||
        session->size() != session_id{}.bytes.size() || !path || !activity ||
        (*activity != "editing" && *activity != "reading" && *activity != "idle") || !seconds) {
        return std::unexpected(failure(errc::corrupt, "invalid presence frame"));
    }
    auto decoded_actor = decode_actor(*actor_encoded);
    if (!decoded_actor) {
        return std::unexpected(decoded_actor.error());
    }
    auto output = liquid::presence{.actor = std::move(*decoded_actor),
                                   .path = *path,
                                   .activity = *activity == "editing"   ? presence_activity::editing
                                               : *activity == "reading" ? presence_activity::reading
                                                                        : presence_activity::idle,
                                   .at = std::chrono::sys_seconds{std::chrono::seconds{*seconds}}};
    std::ranges::copy(*session, output.session.bytes.begin());
    if (range_encoded && !std::holds_alternative<cbor::null>(range_encoded->data)) {
        auto range = decode_range(*range_encoded);
        if (!range) {
            return std::unexpected(range.error());
        }
        output.range = *range;
    }
    return output;
}
auto encode_entry(const working_entry& entry) -> cbor::value {
    auto children = cbor::value::array{};
    for (const auto& [name, child] : entry.entries)
        children.push_back(map({{"name", cbor::value{name}}, {"entry", encode_entry(child)}}));
    auto nested = cbor::value{cbor::null{}};
    if (entry.nested) {
        nested = map(
            {{"repo", bytes(entry.nested->repo_id)}, {"id", digest_value(entry.nested->id.value)}});
    }
    return map({{"kind", cbor::value{static_cast<std::uint64_t>(entry.kind)}},
                {"node", bytes(entry.node)},
                {"mode", cbor::value{std::uint64_t{entry.mode}}},
                {"object", digest_value(entry.object)},
                {"entries", cbor::value{std::move(children)}},
                {"nested", std::move(nested)}});
}
auto decode_entry(const cbor::value& value) -> result<working_entry> {
    const auto* kind = find(value, "kind");
    const auto* node = find(value, "node");
    const auto* mode = find(value, "mode");
    const auto* object = find(value, "object");
    const auto* children = find(value, "entries");
    const auto* nested = find(value, "nested");
    const auto* type = kind ? std::get_if<std::uint64_t>(&kind->data) : nullptr;
    const auto* identity = node ? std::get_if<cbor::value::bytes>(&node->data) : nullptr;
    const auto* bits = mode ? std::get_if<std::uint64_t>(&mode->data) : nullptr;
    const auto* items = children ? std::get_if<cbor::value::array>(&children->data) : nullptr;
    if (!type || *type > 2 || !identity || identity->size() != 16 || !bits || *bits > UINT32_MAX ||
        !object || !items)
        return std::unexpected(
            failure(errc::corrupt, "invalid directory entry", "repository document"));
    auto reference = decode_digest(*object);
    if (!reference)
        return std::unexpected(reference.error());
    working_entry output{
        static_cast<entry_kind>(*type), {}, static_cast<std::uint32_t>(*bits), *reference, {}};
    std::ranges::copy(*identity, output.node.begin());
    if (nested && !std::holds_alternative<cbor::null>(nested->data)) {
        const auto* repository = find(*nested, "repo");
        const auto* id = find(*nested, "id");
        const auto* repository_bytes =
            repository ? std::get_if<cbor::value::bytes>(&repository->data) : nullptr;
        auto change = id ? decode_digest(*id)
                         : result<digest>{std::unexpected(failure(
                               errc::corrupt, "invalid nested reference", "repository document"))};
        if (!repository_bytes || repository_bytes->size() != node_id{}.size() || !change) {
            return std::unexpected(
                failure(errc::corrupt, "invalid nested reference", "repository document"));
        }
        auto repository_id = node_id{};
        std::ranges::copy(*repository_bytes, repository_id.begin());
        output.nested = repo_ref{repository_id, change_id{*change}};
    }
    for (const auto& child : *items) {
        const auto* name = find(child, "name");
        const auto* nested = find(child, "entry");
        const auto* text = name ? std::get_if<std::string>(&name->data) : nullptr;
        if (!text || !nested || output.entries.contains(*text))
            return std::unexpected(
                failure(errc::corrupt, "invalid directory child", "repository document"));
        auto decoded = decode_entry(*nested);
        if (!decoded)
            return std::unexpected(decoded.error());
        output.entries.emplace(*text, std::move(*decoded));
    }
    return output;
}
auto encode_repository_document(const repository_document& value)
    -> result<std::vector<std::byte>> {
    auto refs = cbor::value::array{};
    for (const auto& [name, id] : value.refs)
        refs.push_back(map({{"name", cbor::value{name}}, {"id", digest_value(id.value)}}));
    auto metadata = cbor::value::map{};
    for (const auto& [key, value] : value.metadata)
        metadata.emplace_back(cbor::value{key}, bytes(value));
    auto dirs = cbor::value::array{};
    for (const auto& [node, document] : value.dirs)
        dirs.push_back(map({{"node", bytes(node)}, {"document", digest_value(document)}}));
    auto nested_entries = cbor::value::array{};
    auto retained_nested = std::set<std::string>{};
    const auto collect_nested_entries = [&](const auto& self, const working_entry& entry) -> void {
        if (entry.kind == entry_kind::nested_repository) {
            if (!retained_nested.insert(to_string(entry.object)).second) {
                return;
            }
            nested_entries.push_back(
                map({{"object", digest_value(entry.object)}, {"entry", encode_entry(entry)}}));
            return;
        }
        for (const auto& [name, child] : entry.entries) {
            static_cast<void>(name);
            self(self, child);
        }
    };
    collect_nested_entries(collect_nested_entries, value.root);
    auto ops = cbor::value::array{};
    for (const auto& operation : value.operations) {
        auto entries = cbor::value::array{};
        for (const auto& effect : operation.entries) {
            entries.push_back(
                map({{"path", cbor::value{effect.path}},
                     {"before",
                      effect.before ? encode_entry(*effect.before) : cbor::value{cbor::null{}}},
                     {"after",
                      effect.after ? encode_entry(*effect.after) : cbor::value{cbor::null{}}}}));
        }
        auto refs = cbor::value::array{};
        for (const auto& effect : operation.refs) {
            refs.push_back(map(
                {{"name", cbor::value{effect.name}},
                 {"before",
                  effect.before ? digest_value(effect.before->value) : cbor::value{cbor::null{}}},
                 {"after",
                  effect.after ? digest_value(effect.after->value) : cbor::value{cbor::null{}}}}));
        }
        auto locks = cbor::value::array{};
        for (const auto& effect : operation.locks) {
            locks.push_back(map({{"path", cbor::value{effect.path}},
                                 {"before", cbor::value{effect.before}},
                                 {"after", cbor::value{effect.after}}}));
        }
        auto operation_metadata = cbor::value::map{};
        for (const auto& [name, value] : operation.metadata) {
            operation_metadata.emplace_back(name, value);
        }
        ops.push_back(
            map({{"id", digest_value(operation.id.value)},
                 {"kind", cbor::value{operation.kind}},
                 {"from", cbor::value{operation.from}},
                 {"to", cbor::value{operation.to}},
                 {"node", bytes(operation.node)},
                 {"by", actor_value(operation.by)},
                 {"at", cbor::value{operation.at.time_since_epoch().count()}},
                 {"agent", cbor::value{operation.agent}},
                 {"session", bytes(operation.session.bytes)},
                 {"prior",
                  operation.prior ? encode_entry(*operation.prior) : cbor::value{cbor::null{}}},
                 {"entries", cbor::value{std::move(entries)}},
                 {"refs", cbor::value{std::move(refs)}},
                 {"locks", cbor::value{std::move(locks)}},
                 {"head_before",
                  operation.head_before ? digest_value(operation.head_before->value)
                                        : cbor::value{cbor::null{}}},
                 {"head_after",
                  operation.head_after ? digest_value(operation.head_after->value)
                                       : cbor::value{cbor::null{}}},
                 {"metadata", cbor::value{std::move(operation_metadata)}}}));
    }
    auto locks = cbor::value::array{};
    for (const auto& lock : value.locks) {
        locks.push_back(lock_value(lock));
    }
    auto leases = cbor::value::array{};
    for (const auto& lease : value.leases) {
        auto fields = cbor::value::map{
            {"path", cbor::value{lease.claim.path}},
            {"kind",
             cbor::value{lease.claim.kind == lease_kind::file     ? "file"
                         : lease.claim.kind == lease_kind::symbol ? "symbol"
                                                                  : "range"}},
            {"symbol",
             lease.claim.symbol ? cbor::value{*lease.claim.symbol} : cbor::value{cbor::null{}}},
            {"range",
             lease.claim.range ? range_value(*lease.claim.range) : cbor::value{cbor::null{}}},
            {"by", actor_value(lease.by)},
            {"session", bytes(lease.session.bytes)},
            {"since", cbor::value{lease.since.time_since_epoch().count()}},
            {"expires", cbor::value{lease.expires.time_since_epoch().count()}},
            {"authority",
             cbor::value{lease.authority == lease_authority::lake ? "lake" : "advisory"}},
        };
        leases.emplace_back(cbor::value{std::move(fields)});
    }
    auto epochs = cbor::value::array{};
    for (const auto& epoch : value.epochs) {
        auto heads = cbor::value::array{};
        for (const auto& document : epoch.heads) {
            auto values = cbor::value::array{};
            for (const auto& head : document.values) {
                values.push_back(bytes(head));
            }
            heads.push_back(map({{"directory", bytes(document.directory)},
                                 {"values", cbor::value{std::move(values)}}}));
        }
        epochs.push_back(map({{"n", cbor::value{epoch.n}},
                              {"heads", cbor::value{std::move(heads)}},
                              {"at", cbor::value{epoch.at.time_since_epoch().count()}},
                              {"document_bytes", cbor::value{epoch.document_bytes}}}));
    }
    auto conflicts = cbor::value::array{};
    for (const auto& conflict : value.conflict_values) {
        if (conflict.id.alg != algorithm{}) {
            conflicts.push_back(
                map({{"path", cbor::value{conflict.path}},
                     {"id", digest_value(conflict.id)},
                     {"selected", digest_value(conflict.selected)},
                     {"selected_kind",
                      cbor::value{static_cast<std::uint64_t>(conflict.selected_kind)}}}));
        } else {
            auto references = cbor::value::array{};
            for (const auto& reference : conflict.legacy_refs) {
                references.push_back(digest_value(reference));
            }
            conflicts.push_back(map({{"path", cbor::value{conflict.path}},
                                     {"refs", cbor::value{std::move(references)}}}));
        }
    }
    auto head = value.head ? digest_value(value.head->value) : cbor::value{cbor::null{}};
    auto drips = cbor::value::array{};
    for (const auto& drip : value.drips) {
        drips.push_back(map({{"author", actor_value(drip.author)},
                             {"body", bytes(drip.body)},
                             {"sig", bytes(drip.signature)}}));
    }
    auto encryption_paths = cbor::value::array{};
    for (const auto& rule : value.encryption_rules) {
        encryption_paths.push_back(
            map({{"path", cbor::value{rule.path}},
                 {"key_id", cbor::value{rule.key_id}},
                 {"mode", cbor::value{rule.mode == encryption_mode::leaf ? "leaf" : "file"}},
                 {"encrypted_regex", cbor::value{rule.encrypted_regex}}}));
    }
    auto approved_builders = cbor::value::array{};
    for (const auto& builder : value.policy.approved_builders) {
        approved_builders.push_back(cbor::value{builder});
    }
    auto policy_rules = cbor::value::array{};
    for (const auto& rule : value.policy.rules) {
        auto paths = cbor::value::array{};
        for (const auto& path : rule.paths) {
            paths.push_back(cbor::value{path});
        }
        auto requirements = cbor::value::array{};
        for (const auto& requirement : rule.require) {
            auto authorities = cbor::value::array{};
            for (const auto& authority : requirement.from) {
                authorities.push_back(cbor::value{authority});
            }
            requirements.push_back(
                map({{"profile", cbor::value{requirement.profile}},
                     {"from", cbor::value{std::move(authorities)}},
                     {"min_count", cbor::value{std::uint64_t{requirement.min_count}}}}));
        }
        const auto scope = rule.scope == policy_scope::paths       ? "paths"
                           : rule.scope == policy_scope::toolchain ? "toolchain"
                                                                   : "all inputs";
        const auto on_missing = rule.on_missing == policy_on_missing::refuse ? "refuse" : "warn";
        policy_rules.push_back(map({{"scope", cbor::value{scope}},
                                    {"paths", cbor::value{std::move(paths)}},
                                    {"require", cbor::value{std::move(requirements)}},
                                    {"on_missing", cbor::value{on_missing}}}));
    }
    auto retention_rules = cbor::value::array{};
    for (const auto& configured : value.retention.rules) {
        std::visit(
            [&](const auto& rule) {
                using rule_type = std::decay_t<decltype(rule)>;
                if constexpr (std::is_same_v<rule_type, ref_retention_rule>) {
                    auto fields = cbor::value::map{{"type", "refs"},
                                                   {"name", cbor::value{rule.name}},
                                                   {"glob", cbor::value{rule.glob}}};
                    if (rule.duration) {
                        fields.emplace_back(
                            "duration_s",
                            cbor::value{static_cast<std::uint64_t>(rule.duration->count())});
                    } else {
                        fields.emplace_back("forever", cbor::value{true});
                    }
                    retention_rules.emplace_back(cbor::value{std::move(fields)});
                } else if constexpr (std::is_same_v<rule_type, result_retention_rule>) {
                    retention_rules.push_back(
                        map({{"type", "results"},
                             {"name", cbor::value{rule.name}},
                             {"duration_s",
                              cbor::value{static_cast<std::uint64_t>(rule.base.count())}}}));
                } else {
                    retention_rules.push_back(map({{"type", "size_cap"},
                                                   {"name", cbor::value{rule.name}},
                                                   {"bytes", cbor::value{rule.bytes}}}));
                }
            },
            configured);
    }
    const auto bucket_value = [](const token_bucket_config& bucket) {
        return map(
            {{"rate_per_s", cbor::value{bucket.rate_per_s}}, {"burst", cbor::value{bucket.burst}}});
    };
    auto actor_overrides = cbor::value::array{};
    for (const auto& [actor, bucket] : value.flow.actor_overrides) {
        actor_overrides.push_back(
            map({{"taap_id", cbor::value{actor}}, {"bucket", bucket_value(bucket)}}));
    }
    auto flow = map({{"per_actor", bucket_value(value.flow.per_actor)},
                     {"per_document", bucket_value(value.flow.per_document)},
                     {"per_repository", bucket_value(value.flow.per_repository)},
                     {"actor_overrides", cbor::value{std::move(actor_overrides)}},
                     {"max_update_bytes", cbor::value{value.flow.max_update_bytes}},
                     {"max_unacknowledged", cbor::value{value.flow.max_unacknowledged}},
                     {"pause_above", cbor::value{value.flow.pause_above}},
                     {"resume_below", cbor::value{value.flow.resume_below}}});
    auto remote = cbor::value{cbor::null{}};
    if (value.remote) {
        auto headers = cbor::value::array{};
        for (const auto& [name, header_value] : value.remote->headers) {
            headers.push_back(
                map({{"name", cbor::value{name}}, {"value", cbor::value{header_value}}}));
        }
        auto tls = cbor::value::map{{"enabled", cbor::value{value.remote->tls.enabled}},
                                    {"verify_peer", cbor::value{value.remote->tls.verify_peer}}};
        const auto add_path = [&](std::string name,
                                  const std::optional<std::filesystem::path>& path) {
            tls.emplace_back(std::move(name),
                             path ? cbor::value{path->string()} : cbor::value{cbor::null{}});
        };
        add_path("certificate_authority", value.remote->tls.certificate_authority);
        add_path("client_certificate", value.remote->tls.client_certificate);
        add_path("client_key", value.remote->tls.client_key);
        remote = map({{"endpoint", cbor::value{value.remote->endpoint}},
                      {"instance_name", cbor::value{value.remote->instance_name}},
                      {"backend", cbor::value{static_cast<std::uint64_t>(value.remote->backend)}},
                      {"tls", cbor::value{std::move(tls)}},
                      {"headers", cbor::value{std::move(headers)}},
                      {"deadline_ms",
                       cbor::value{static_cast<std::uint64_t>(value.remote->deadline.count())}},
                      {"batch_blob_limit", cbor::value{value.remote->batch_blob_limit}},
                      {"publish", cbor::value{value.remote->publish}}});
    }
    auto data_databases = cbor::value::array{};
    for (const auto& database : value.data.databases) {
        data_databases.push_back(
            map({{"name", cbor::value{database.name}},
                 {"url_env", cbor::value{database.url_env}},
                 {"base_url", cbor::value{database.base_url}},
                 {"mechanism", cbor::value{std::string{to_string(database.mechanism)}}}}));
    }
    auto data_branches = cbor::value::array{};
    for (const auto& branch : value.data_branches) {
        data_branches.push_back(
            map({{"id", cbor::value{branch.record.id}},
                 {"session", bytes(branch.record.session.bytes)},
                 {"database", cbor::value{branch.record.database}},
                 {"url", cbor::value{branch.record.url}},
                 {"mechanism", cbor::value{std::string{to_string(branch.record.mechanism)}}},
                 {"created_at", cbor::value{branch.record.created_at.time_since_epoch().count()}},
                 {"base_url", cbor::value{branch.base_url}},
                 {"base_fingerprint", cbor::value{branch.base_fingerprint}}}));
    }
    auto restart_hook = cbor::value::array{};
    for (const auto& argument : value.hot_update.restart_hook) {
        restart_hook.push_back(cbor::value{argument});
    }
    auto hot_update_targets = cbor::value::array{};
    for (const auto& target : value.hot_update.targets) {
        hot_update_targets.push_back(map({{"name", cbor::value{target.name}},
                                          {"endpoint", cbor::value{target.endpoint.string()}}}));
    }
    auto deploy_environments = cbor::value::array{};
    for (const auto& [name, environment] : value.deploy.environments) {
        auto variables = cbor::value::array{};
        for (const auto& [key, variable] : environment.env) {
            variables.push_back(
                map({{"name", cbor::value{key}}, {"value", cbor::value{variable}}}));
        }
        auto secrets = cbor::value::array{};
        for (const auto& secret : environment.secrets) {
            secrets.push_back(cbor::value{secret});
        }
        deploy_environments.push_back(map(
            {{"name", cbor::value{name}},
             {"domain",
              environment.domain ? cbor::value{*environment.domain} : cbor::value{cbor::null{}}},
             {"env", cbor::value{std::move(variables)}},
             {"secrets", cbor::value{std::move(secrets)}}}));
    }
    auto deploy =
        map({{"control_plane", cbor::value{value.deploy.control_plane}},
             {"environments", cbor::value{std::move(deploy_environments)}},
             {"on_push",
              cbor::value{value.deploy.on_push == deploy_on_push::preview ? "preview" : "none"}}});
    auto config =
        map({{"encrypt", map({{"paths", cbor::value{std::move(encryption_paths)}}})},
             {"locks", lock_config_value(value.lock_rules)},
             {"cache", map({{"publish", cbor::value{value.cache_publish}}})},
             {"verify", map({{"rate", cbor::value{std::uint64_t{value.verification_rate}}}})},
             {"agents",
              [&] {
                  auto actors = cbor::value::array{};
                  for (const auto& actor : value.agents.actors) {
                      actors.push_back(cbor::value{actor});
                  }
                  return map({{"require_base", cbor::value{value.agents.require_base}},
                              {"actors", cbor::value{std::move(actors)}}});
              }()},
             {"policy",
              map({{"approved_builders", cbor::value{std::move(approved_builders)}},
                   {"rules", cbor::value{std::move(policy_rules)}}})},
             {"retention", map({{"rules", cbor::value{std::move(retention_rules)}}})},
             {"data",
              map({{"databases", cbor::value{std::move(data_databases)}},
                   {"branches", cbor::value{std::move(data_branches)}}})},
             {"hot_update",
              map({{"restart_hook", cbor::value{std::move(restart_hook)}},
                   {"targets", cbor::value{std::move(hot_update_targets)}}})},
             {"deploy", std::move(deploy)},
             {"flow", std::move(flow)},
             {"remote", std::move(remote)}});
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "repository-document"},
                             {"repo", bytes(value.repository)},
                             {"root_node", bytes(value.root.node)},
                             {"root_mode", cbor::value{std::uint64_t{value.root.mode}}},
                             {"root_ref", digest_value(value.root.object)},
                             {"refs", cbor::value{std::move(refs)}},
                             {"metadata", cbor::value{std::move(metadata)}},
                             {"dirs", cbor::value{std::move(dirs)}},
                             {"nested_entries", cbor::value{std::move(nested_entries)}},
                             {"ops", cbor::value{std::move(ops)}},
                             {"head", std::move(head)},
                             {"locks", cbor::value{std::move(locks)}},
                             {"leases", cbor::value{std::move(leases)}},
                             {"epochs", cbor::value{std::move(epochs)}},
                             {"conflicts", cbor::value{std::move(conflicts)}},
                             {"config", std::move(config)},
                             {"drips", cbor::value{std::move(drips)}},
                             {"pending", cbor::value{value.pending_drips}}}));
}
auto decode_repository_document(std::span<const std::byte> input) -> result<repository_document> {
    auto decoded = cbor::decode(input);
    if (!decoded)
        return std::unexpected(decoded.error());
    const auto* version = find(*decoded, "v");
    const auto* kind = find(*decoded, "kind");
    const auto* repo = find(*decoded, "repo");
    const auto* root = find(*decoded, "root");
    const auto* root_node = find(*decoded, "root_node");
    const auto* root_mode = find(*decoded, "root_mode");
    const auto* root_ref = find(*decoded, "root_ref");
    const auto* refs = find(*decoded, "refs");
    const auto* metadata = find(*decoded, "metadata");
    const auto* dirs = find(*decoded, "dirs");
    const auto* nested = find(*decoded, "nested_entries");
    const auto* ops = find(*decoded, "ops");
    const auto* head = find(*decoded, "head");
    const auto* locks = find(*decoded, "locks");
    const auto* leases = find(*decoded, "leases");
    const auto* epochs = find(*decoded, "epochs");
    const auto* conflicts = find(*decoded, "conflicts");
    const auto* config = find(*decoded, "config");
    const auto* drip_values = find(*decoded, "drips");
    const auto* pending = find(*decoded, "pending");
    const auto* v = version ? std::get_if<std::uint64_t>(&version->data) : nullptr;
    const auto* title = kind ? std::get_if<std::string>(&kind->data) : nullptr;
    const auto* id = repo ? std::get_if<cbor::value::bytes>(&repo->data) : nullptr;
    const auto* ref_values = refs ? std::get_if<cbor::value::array>(&refs->data) : nullptr;
    const auto* metadata_values =
        metadata ? std::get_if<cbor::value::map>(&metadata->data) : nullptr;
    const auto* dir_values = dirs ? std::get_if<cbor::value::array>(&dirs->data) : nullptr;
    const auto* nested_values = nested ? std::get_if<cbor::value::array>(&nested->data) : nullptr;
    const auto* op_values = ops ? std::get_if<cbor::value::array>(&ops->data) : nullptr;
    const auto* lock_values = locks ? std::get_if<cbor::value::array>(&locks->data) : nullptr;
    const auto* conflict_values =
        conflicts ? std::get_if<cbor::value::array>(&conflicts->data) : nullptr;
    const auto* saved_drips =
        drip_values ? std::get_if<cbor::value::array>(&drip_values->data) : nullptr;
    const auto* drips = pending ? std::get_if<std::uint64_t>(&pending->data) : nullptr;
    const auto paged_root = !root && root_node && root_mode && root_ref;
    if (!v || *v != 1 || !title || *title != "repository-document" || !id || id->size() != 16 ||
        (!root && !paged_root) || !ref_values || !dir_values || (paged_root && !nested_values) ||
        !op_values || !head || !lock_values || !saved_drips || !drips)
        return std::unexpected(
            failure(errc::corrupt, "invalid repository document", "LIQUID_REPO"));
    repository_document output{};
    std::ranges::copy(*id, output.repository.begin());
    if (root) {
        auto root_entry = decode_entry(*root);
        if (!root_entry) {
            return std::unexpected(root_entry.error());
        }
        output.root = std::move(*root_entry);
    } else {
        const auto* node = std::get_if<cbor::value::bytes>(&root_node->data);
        const auto* mode = std::get_if<std::uint64_t>(&root_mode->data);
        if (!node || node->size() != output.root.node.size() || !mode || *mode > UINT32_MAX) {
            return std::unexpected(
                failure(errc::corrupt, "invalid paged repository root", "LIQUID_REPO"));
        }
        auto reference = decode_digest(*root_ref);
        if (!reference) {
            return std::unexpected(reference.error());
        }
        output.root = {
            entry_kind::directory, {}, static_cast<std::uint32_t>(*mode), *reference, {}, {}};
        std::ranges::copy(*node, output.root.node.begin());
        output.root_is_paged = true;
    }
    output.pending_drips = *drips;
    for (const auto& item : *ref_values) {
        const auto* name = find(item, "name");
        const auto* reference = find(item, "id");
        const auto* text = name ? std::get_if<std::string>(&name->data) : nullptr;
        if (!text || !reference || output.refs.contains(*text))
            return std::unexpected(
                failure(errc::corrupt, "invalid reference", "repository document"));
        auto digest = decode_digest(*reference);
        if (!digest)
            return std::unexpected(digest.error());
        output.refs.emplace(*text, change_id{*digest});
    }
    if (metadata_values) {
        for (const auto& [key, value] : *metadata_values) {
            const auto* name = std::get_if<std::string>(&key.data);
            const auto* data = std::get_if<cbor::value::bytes>(&value.data);
            if (!name || !data || !output.metadata.emplace(*name, *data).second)
                return std::unexpected(
                    failure(errc::corrupt, "invalid repository metadata", "repository document"));
        }
    }
    for (const auto& item : *dir_values) {
        const auto* node = find(item, "node");
        const auto* document = find(item, "document");
        const auto* raw = node ? std::get_if<cbor::value::bytes>(&node->data) : nullptr;
        if (!raw || raw->size() != 16 || !document)
            return std::unexpected(
                failure(errc::corrupt, "invalid directory reference", "repository document"));
        node_id identity{};
        std::ranges::copy(*raw, identity.begin());
        auto digest = decode_digest(*document);
        if (!digest)
            return std::unexpected(digest.error());
        output.dirs.emplace(identity, *digest);
    }
    for (const auto& item : nested_values ? *nested_values : cbor::value::array{}) {
        const auto* object = find(item, "object");
        const auto* entry = find(item, "entry");
        if (!object || !entry) {
            return std::unexpected(
                failure(errc::corrupt, "invalid nested repository entry", "repository document"));
        }
        auto decoded_entry = decode_entry(*entry);
        if (!decoded_entry || decoded_entry->kind != entry_kind::nested_repository) {
            return std::unexpected(decoded_entry ? failure(errc::corrupt,
                                                           "invalid nested repository kind",
                                                           "repository document")
                                                 : decoded_entry.error());
        }
        auto reference = decode_digest(*object);
        if (!reference) {
            return std::unexpected(reference.error());
        }
        if (!output.nested_entries.emplace(to_string(*reference), std::move(*decoded_entry))
                 .second) {
            return std::unexpected(
                failure(errc::corrupt, "duplicate nested repository entry", "repository document"));
        }
    }
    for (const auto& item : *op_values) {
        const auto* operation_id = find(item, "id");
        const auto* operation_kind = find(item, "kind");
        const auto* from = find(item, "from");
        const auto* to = find(item, "to");
        const auto* node = find(item, "node");
        const auto* by = find(item, "by");
        const auto* at = find(item, "at");
        const auto* agent = find(item, "agent");
        const auto* prior = find(item, "prior");
        const auto* session = find(item, "session");
        const auto* entry_effects = find(item, "entries");
        const auto* ref_effects = find(item, "refs");
        const auto* lock_effects = find(item, "locks");
        const auto* head_before = find(item, "head_before");
        const auto* head_after = find(item, "head_after");
        const auto* operation_metadata = find(item, "metadata");
        const auto* kind_text =
            operation_kind ? std::get_if<std::string>(&operation_kind->data) : nullptr;
        const auto* from_text = from ? std::get_if<std::string>(&from->data) : nullptr;
        const auto* to_text = to ? std::get_if<std::string>(&to->data) : nullptr;
        const auto* node_bytes = node ? std::get_if<cbor::value::bytes>(&node->data) : nullptr;
        if (!operation_id || !kind_text || !from_text || !to_text || !prior)
            return std::unexpected(
                failure(errc::corrupt, "invalid operation", "repository document"));
        auto digest = decode_digest(*operation_id);
        if (!digest)
            return std::unexpected(digest.error());
        auto previous = std::optional<working_entry>{};
        if (!std::holds_alternative<cbor::null>(prior->data)) {
            auto entry = decode_entry(*prior);
            if (!entry)
                return std::unexpected(entry.error());
            previous = std::move(*entry);
        }
        node_id identity{};
        if (node_bytes) {
            if (node_bytes->size() != identity.size())
                return std::unexpected(
                    failure(errc::corrupt, "invalid operation node", "repository document"));
            std::ranges::copy(*node_bytes, identity.begin());
        }
        auto operation_actor = actor{};
        auto operation_time = std::chrono::sys_seconds{};
        if (by || at) {
            if (!by || !at) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid operation metadata", "repository document"));
            }
            auto seconds = std::optional<std::int64_t>{};
            if (const auto* signed_seconds = std::get_if<std::int64_t>(&at->data)) {
                seconds = *signed_seconds;
            } else if (const auto* unsigned_seconds = std::get_if<std::uint64_t>(&at->data);
                       unsigned_seconds &&
                       *unsigned_seconds <=
                           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                seconds = static_cast<std::int64_t>(*unsigned_seconds);
            }
            if (!seconds) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid operation timestamp", "repository document"));
            }
            auto decoded_actor = decode_actor(*by);
            if (!decoded_actor) {
                return std::unexpected(decoded_actor.error());
            }
            operation_actor = std::move(*decoded_actor);
            operation_time = std::chrono::sys_seconds{std::chrono::seconds{*seconds}};
        }
        auto decoded_operation = stored_operation{};
        decoded_operation.id = op_id{*digest};
        decoded_operation.kind = *kind_text;
        decoded_operation.from = *from_text;
        decoded_operation.to = *to_text;
        decoded_operation.node = identity;
        decoded_operation.prior = std::move(previous);
        decoded_operation.by = std::move(operation_actor);
        decoded_operation.at = operation_time;
        if (agent) {
            const auto* marked = std::get_if<bool>(&agent->data);
            if (!marked) {
                return std::unexpected(failure(
                    errc::corrupt, "invalid operation agent marker", "repository document"));
            }
            decoded_operation.agent = *marked;
        }
        if (session) {
            const auto* raw = std::get_if<cbor::value::bytes>(&session->data);
            if (!raw || raw->size() != decoded_operation.session.bytes.size()) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid operation session", "repository document"));
            }
            std::ranges::copy(*raw, decoded_operation.session.bytes.begin());
        }
        if (operation_metadata) {
            const auto* values = std::get_if<cbor::value::map>(&operation_metadata->data);
            if (!values) {
                return std::unexpected(failure(
                    errc::corrupt, "invalid operation metadata fields", "repository document"));
            }
            for (const auto& [key, value] : *values) {
                const auto* name = std::get_if<std::string>(&key.data);
                const auto* text = std::get_if<std::string>(&value.data);
                if (!name || !text || !decoded_operation.metadata.emplace(*name, *text).second) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid operation metadata fields", "repository document"));
                }
            }
        }
        const auto decode_entry_effect =
            [&](const cbor::value& effect) -> result<stored_entry_effect> {
            const auto* path_value = find(effect, "path");
            const auto* before_value = find(effect, "before");
            const auto* after_value = find(effect, "after");
            const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
            if (!path || !before_value || !after_value) {
                return std::unexpected(failure(
                    errc::corrupt, "invalid operation entry effect", "repository document"));
            }
            auto decoded = stored_entry_effect{.path = *path};
            if (!std::holds_alternative<cbor::null>(before_value->data)) {
                auto entry = decode_entry(*before_value);
                if (!entry) {
                    return std::unexpected(entry.error());
                }
                decoded.before = std::move(*entry);
            }
            if (!std::holds_alternative<cbor::null>(after_value->data)) {
                auto entry = decode_entry(*after_value);
                if (!entry) {
                    return std::unexpected(entry.error());
                }
                decoded.after = std::move(*entry);
            }
            return decoded;
        };
        if (entry_effects) {
            const auto* effects = std::get_if<cbor::value::array>(&entry_effects->data);
            if (!effects) {
                return std::unexpected(failure(
                    errc::corrupt, "invalid operation entry effects", "repository document"));
            }
            for (const auto& effect : *effects) {
                auto decoded = decode_entry_effect(effect);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                decoded_operation.entries.push_back(std::move(*decoded));
            }
        }
        const auto decode_change =
            [&](const cbor::value& value) -> result<std::optional<change_id>> {
            if (std::holds_alternative<cbor::null>(value.data)) {
                return std::optional<change_id>{};
            }
            auto id = decode_digest(value);
            if (!id) {
                return std::unexpected(id.error());
            }
            return std::optional<change_id>{change_id{*id}};
        };
        if (ref_effects) {
            const auto* effects = std::get_if<cbor::value::array>(&ref_effects->data);
            if (!effects) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid operation ref effects", "repository document"));
            }
            for (const auto& effect : *effects) {
                const auto* name_value = find(effect, "name");
                const auto* before_value = find(effect, "before");
                const auto* after_value = find(effect, "after");
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                if (!name || !before_value || !after_value) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid operation ref effect", "repository document"));
                }
                auto before = decode_change(*before_value);
                auto after = decode_change(*after_value);
                if (!before || !after) {
                    return std::unexpected(before ? after.error() : before.error());
                }
                decoded_operation.refs.push_back({*name, *before, *after});
            }
        }
        if (lock_effects) {
            const auto* effects = std::get_if<cbor::value::array>(&lock_effects->data);
            if (!effects) {
                return std::unexpected(failure(
                    errc::corrupt, "invalid operation lock effects", "repository document"));
            }
            for (const auto& effect : *effects) {
                const auto* path_value = find(effect, "path");
                const auto* before_value = find(effect, "before");
                const auto* after_value = find(effect, "after");
                const auto* path =
                    path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
                const auto* before =
                    before_value ? std::get_if<bool>(&before_value->data) : nullptr;
                const auto* after = after_value ? std::get_if<bool>(&after_value->data) : nullptr;
                if (!path || !before || !after) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid operation lock effect", "repository document"));
                }
                decoded_operation.locks.push_back({*path, *before, *after});
            }
        }
        if (head_before && head_after) {
            auto before = decode_change(*head_before);
            auto after = decode_change(*head_after);
            if (!before || !after) {
                return std::unexpected(before ? after.error() : before.error());
            }
            decoded_operation.head_before = *before;
            decoded_operation.head_after = *after;
        }
        output.operations.push_back(std::move(decoded_operation));
    }
    if (!std::holds_alternative<cbor::null>(head->data)) {
        auto digest = decode_digest(*head);
        if (!digest)
            return std::unexpected(digest.error());
        output.head = change_id{*digest};
    }
    for (const auto& item : *lock_values) {
        if (const auto* legacy = std::get_if<std::string>(&item.data)) {
            output.locks.push_back({.path = *legacy,
                                    .reason = "legacy advisory lock",
                                    .authority = lock_authority::advisory});
            continue;
        }
        auto decoded_lock = decode_lock(item);
        if (!decoded_lock) {
            return std::unexpected(decoded_lock.error());
        }
        output.locks.push_back(std::move(*decoded_lock));
    }
    const auto decode_seconds = [](const cbor::value& value) -> std::optional<std::int64_t> {
        if (const auto* signed_value = std::get_if<std::int64_t>(&value.data)) {
            return *signed_value;
        }
        const auto* unsigned_value = std::get_if<std::uint64_t>(&value.data);
        if (unsigned_value && *unsigned_value <= static_cast<std::uint64_t>(
                                                     std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*unsigned_value);
        }
        return std::nullopt;
    };
    if (leases) {
        const auto* values = std::get_if<cbor::value::array>(&leases->data);
        if (!values) {
            return std::unexpected(failure(errc::corrupt, "invalid leases", "repository document"));
        }
        for (const auto& item : *values) {
            const auto* path_value = find(item, "path");
            const auto* kind_value = find(item, "kind");
            const auto* symbol_value = find(item, "symbol");
            const auto* range_encoded = find(item, "range");
            const auto* by_value = find(item, "by");
            const auto* session_value = find(item, "session");
            const auto* since_value = find(item, "since");
            const auto* expires_value = find(item, "expires");
            const auto* authority_value = find(item, "authority");
            const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
            const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
            const auto* session =
                session_value ? std::get_if<cbor::value::bytes>(&session_value->data) : nullptr;
            const auto* authority =
                authority_value ? std::get_if<std::string>(&authority_value->data) : nullptr;
            const auto since = since_value ? decode_seconds(*since_value) : std::nullopt;
            const auto expires = expires_value ? decode_seconds(*expires_value) : std::nullopt;
            if (!path || path->empty() || !kind ||
                (*kind != "file" && *kind != "symbol" && *kind != "range") || !by_value ||
                !session || session->size() != session_id{}.bytes.size() || !since || !expires ||
                !authority || (*authority != "advisory" && *authority != "lake")) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid lease", "repository document"));
            }
            auto decoded_actor = decode_actor(*by_value);
            if (!decoded_actor) {
                return std::unexpected(decoded_actor.error());
            }
            auto value = semantic_lease{
                .claim = {.path = *path,
                          .kind = *kind == "file"     ? lease_kind::file
                                  : *kind == "symbol" ? lease_kind::symbol
                                                      : lease_kind::range},
                .by = std::move(*decoded_actor),
                .since = std::chrono::sys_seconds{std::chrono::seconds{*since}},
                .expires = std::chrono::sys_seconds{std::chrono::seconds{*expires}},
                .authority =
                    *authority == "lake" ? lease_authority::lake : lease_authority::advisory,
            };
            std::ranges::copy(*session, value.session.bytes.begin());
            if (symbol_value && !std::holds_alternative<cbor::null>(symbol_value->data)) {
                const auto* symbol = std::get_if<std::string>(&symbol_value->data);
                if (!symbol || symbol->empty()) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid lease symbol", "repository document"));
                }
                value.claim.symbol = *symbol;
            }
            if (range_encoded && !std::holds_alternative<cbor::null>(range_encoded->data)) {
                auto range = decode_range(*range_encoded);
                if (!range) {
                    return std::unexpected(range.error());
                }
                value.claim.range = *range;
            }
            output.leases.push_back(std::move(value));
        }
    }
    if (epochs) {
        const auto* values = std::get_if<cbor::value::array>(&epochs->data);
        if (!values) {
            return std::unexpected(failure(errc::corrupt, "invalid epochs", "repository document"));
        }
        for (const auto& item : *values) {
            const auto* number_value = find(item, "n");
            const auto* heads_value = find(item, "heads");
            const auto* at_value = find(item, "at");
            const auto* bytes_value = find(item, "document_bytes");
            const auto* number =
                number_value ? std::get_if<std::uint64_t>(&number_value->data) : nullptr;
            const auto* heads =
                heads_value ? std::get_if<cbor::value::array>(&heads_value->data) : nullptr;
            const auto at = at_value ? decode_seconds(*at_value) : std::nullopt;
            const auto* document_bytes =
                bytes_value ? std::get_if<std::uint64_t>(&bytes_value->data) : nullptr;
            if (!number || !heads || !at || !document_bytes) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid epoch", "repository document"));
            }
            auto value = epoch{.n = *number,
                               .at = std::chrono::sys_seconds{std::chrono::seconds{*at}},
                               .document_bytes = *document_bytes};
            for (const auto& head_value : *heads) {
                const auto* directory_value = find(head_value, "directory");
                const auto* values_value = find(head_value, "values");
                const auto* directory =
                    directory_value ? std::get_if<cbor::value::bytes>(&directory_value->data)
                                    : nullptr;
                const auto* head_values =
                    values_value ? std::get_if<cbor::value::array>(&values_value->data) : nullptr;
                if (!directory || directory->size() != node_id{}.size() || !head_values) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid epoch document heads", "repository document"));
                }
                auto document = document_heads{};
                std::ranges::copy(*directory, document.directory.begin());
                for (const auto& head : *head_values) {
                    const auto* raw = std::get_if<cbor::value::bytes>(&head.data);
                    if (!raw || raw->size() != 32) {
                        return std::unexpected(
                            failure(errc::corrupt, "invalid epoch head", "repository document"));
                    }
                    auto decoded_head = std::array<std::byte, 32>{};
                    std::ranges::copy(*raw, decoded_head.begin());
                    document.values.push_back(decoded_head);
                }
                value.heads.push_back(std::move(document));
            }
            output.epochs.push_back(std::move(value));
        }
    }
    if (conflicts && !conflict_values) {
        return std::unexpected(
            failure(errc::corrupt, "invalid conflict values", "repository document"));
    }
    if (conflict_values) {
        for (const auto& item : *conflict_values) {
            const auto* path_value = find(item, "path");
            const auto* id_value = find(item, "id");
            const auto* selected_value = find(item, "selected");
            const auto* selected_kind_value = find(item, "selected_kind");
            const auto* refs_value = find(item, "refs");
            const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
            const auto* refs =
                refs_value ? std::get_if<cbor::value::array>(&refs_value->data) : nullptr;
            if (!path || path->empty() || (id_value == nullptr) == (refs == nullptr)) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid conflict value", "repository document"));
            }
            auto value = conflict_reference{.path = *path};
            if (id_value) {
                const auto* selected_kind =
                    selected_kind_value ? std::get_if<std::uint64_t>(&selected_kind_value->data)
                                        : nullptr;
                auto decoded_id = decode_digest(*id_value);
                auto decoded_selected =
                    selected_value
                        ? decode_digest(*selected_value)
                        : result<digest>{std::unexpected(failure(
                              errc::corrupt, "invalid conflict selection", "repository document"))};
                const auto valid_selected_kind =
                    selected_kind &&
                    (*selected_kind == static_cast<std::uint64_t>(node_kind::blob) ||
                     *selected_kind == static_cast<std::uint64_t>(node_kind::tree) ||
                     *selected_kind == static_cast<std::uint64_t>(node_kind::repo));
                if (!decoded_id || !decoded_selected || !valid_selected_kind) {
                    if (!decoded_id) {
                        return std::unexpected(decoded_id.error());
                    }
                    if (!decoded_selected) {
                        return std::unexpected(decoded_selected.error());
                    }
                    return std::unexpected(failure(
                        errc::corrupt, "invalid conflict selection", "repository document"));
                }
                value.id = *decoded_id;
                value.selected = *decoded_selected;
                value.selected_kind = static_cast<node_kind>(*selected_kind);
            } else {
                if (refs->size() < 2) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid legacy conflict value", "repository document"));
                }
                for (const auto& reference : *refs) {
                    auto decoded_reference = decode_digest(reference);
                    if (!decoded_reference) {
                        return std::unexpected(decoded_reference.error());
                    }
                    value.legacy_refs.push_back(*decoded_reference);
                }
            }
            output.conflict_values.push_back(std::move(value));
        }
    }
    if (config) {
        const auto* encrypt = find(*config, "encrypt");
        const auto* paths = encrypt ? find(*encrypt, "paths") : nullptr;
        const auto* path_values = paths ? std::get_if<cbor::value::array>(&paths->data) : nullptr;
        if (!encrypt || !path_values) {
            return std::unexpected(
                failure(errc::corrupt, "invalid encryption config", "repository document"));
        }
        for (const auto& item : *path_values) {
            const auto* path = find(item, "path");
            const auto* key_id = find(item, "key_id");
            const auto* mode = find(item, "mode");
            const auto* encrypted_regex = find(item, "encrypted_regex");
            const auto* path_text = path ? std::get_if<std::string>(&path->data) : nullptr;
            const auto* key_text = key_id ? std::get_if<std::string>(&key_id->data) : nullptr;
            const auto* mode_text = mode ? std::get_if<std::string>(&mode->data) : nullptr;
            const auto* regex_text =
                encrypted_regex ? std::get_if<std::string>(&encrypted_regex->data) : nullptr;
            if (!path_text || !key_text || !mode_text ||
                (*mode_text != "leaf" && *mode_text != "file") || !regex_text) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid encryption path rule", "repository document"));
            }
            output.encryption_rules.push_back(
                {*path_text,
                 *key_text,
                 *mode_text == "leaf" ? encryption_mode::leaf : encryption_mode::file,
                 *regex_text});
        }
        if (const auto* locks_config = find(*config, "locks")) {
            auto decoded_locks = decode_lock_config(*locks_config);
            if (!decoded_locks) {
                return std::unexpected(decoded_locks.error());
            }
            output.lock_rules = std::move(*decoded_locks);
        }
        if (const auto* cache = find(*config, "cache")) {
            const auto* publish = find(*cache, "publish");
            const auto* enabled = publish ? std::get_if<bool>(&publish->data) : nullptr;
            if (!enabled) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid cache config", "repository document"));
            }
            output.cache_publish = *enabled;
        }
        if (const auto* verify = find(*config, "verify")) {
            const auto* rate = find(*verify, "rate");
            const auto* value = rate ? std::get_if<std::uint64_t>(&rate->data) : nullptr;
            if (!value || *value == 0 || *value > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid verification config", "repository document"));
            }
            output.verification_rate = static_cast<std::uint32_t>(*value);
        }
        if (const auto* agents = find(*config, "agents")) {
            const auto* require_base_value = find(*agents, "require_base");
            const auto* actors_value = find(*agents, "actors");
            const auto* require_base =
                require_base_value ? std::get_if<bool>(&require_base_value->data) : nullptr;
            const auto* actors =
                actors_value ? std::get_if<cbor::value::array>(&actors_value->data) : nullptr;
            if (!require_base || !actors) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid agents config", "repository document"));
            }
            output.agents.require_base = *require_base;
            for (const auto& item : *actors) {
                const auto* actor = std::get_if<std::string>(&item.data);
                if (!actor || actor->empty()) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid agent actor", "repository document"));
                }
                output.agents.actors.push_back(*actor);
            }
        }
        if (const auto* data = find(*config, "data")) {
            const auto* databases_value = find(*data, "databases");
            const auto* branches_value = find(*data, "branches");
            const auto* databases =
                databases_value ? std::get_if<cbor::value::array>(&databases_value->data) : nullptr;
            const auto* branches =
                branches_value ? std::get_if<cbor::value::array>(&branches_value->data) : nullptr;
            if (!databases || !branches) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid data config", "repository document"));
            }
            const auto mechanism = [](const cbor::value* value,
                                      bool allow_automatic) -> result<data_branch_mechanism> {
                const auto* text = value ? std::get_if<std::string>(&value->data) : nullptr;
                if (text && *text == "template") {
                    return data_branch_mechanism::template_copy;
                }
                if (text && *text == "reflink") {
                    return data_branch_mechanism::reflink;
                }
                if (text && *text == "auto" && allow_automatic) {
                    return data_branch_mechanism::automatic;
                }
                return std::unexpected(
                    failure(errc::corrupt, "invalid data branch mechanism", "repository document"));
            };
            for (const auto& item : *databases) {
                const auto* name_value = find(item, "name");
                const auto* env_value = find(item, "url_env");
                const auto* url_value = find(item, "base_url");
                const auto* mechanism_value = find(item, "mechanism");
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                const auto* env = env_value ? std::get_if<std::string>(&env_value->data) : nullptr;
                const auto* url = url_value ? std::get_if<std::string>(&url_value->data) : nullptr;
                auto decoded_mechanism = mechanism(mechanism_value, true);
                if (!name || !env || !url || !decoded_mechanism) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid data database", "repository document"));
                }
                output.data.databases.push_back({*name, *env, *url, *decoded_mechanism});
            }
            for (const auto& item : *branches) {
                const auto* id_value = find(item, "id");
                const auto* session_value = find(item, "session");
                const auto* database_value = find(item, "database");
                const auto* url_value = find(item, "url");
                const auto* mechanism_value = find(item, "mechanism");
                const auto* created_value = find(item, "created_at");
                const auto* base_url_value = find(item, "base_url");
                const auto* fingerprint_value = find(item, "base_fingerprint");
                const auto* id = id_value ? std::get_if<std::string>(&id_value->data) : nullptr;
                const auto* session =
                    session_value ? std::get_if<cbor::value::bytes>(&session_value->data) : nullptr;
                const auto* database =
                    database_value ? std::get_if<std::string>(&database_value->data) : nullptr;
                const auto* url = url_value ? std::get_if<std::string>(&url_value->data) : nullptr;
                const auto* base_url =
                    base_url_value ? std::get_if<std::string>(&base_url_value->data) : nullptr;
                const auto* fingerprint = fingerprint_value
                                              ? std::get_if<std::string>(&fingerprint_value->data)
                                              : nullptr;
                auto decoded_mechanism = mechanism(mechanism_value, false);
                auto created = std::optional<std::int64_t>{};
                if (created_value) {
                    if (const auto* signed_value =
                            std::get_if<std::int64_t>(&created_value->data)) {
                        created = *signed_value;
                    } else if (const auto* unsigned_value =
                                   std::get_if<std::uint64_t>(&created_value->data);
                               unsigned_value &&
                               *unsigned_value <= static_cast<std::uint64_t>(
                                                      std::numeric_limits<std::int64_t>::max())) {
                        created = static_cast<std::int64_t>(*unsigned_value);
                    }
                }
                if (!id || id->empty() || !session ||
                    session->size() != session_id{}.bytes.size() || !database ||
                    database->empty() || !url || url->empty() || !decoded_mechanism || !created ||
                    !base_url || base_url->empty() || !fingerprint || fingerprint->empty()) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid data branch", "repository document"));
                }
                auto stored = repository_detail::stored_data_branch{};
                stored.record.id = *id;
                std::ranges::copy(*session, stored.record.session.bytes.begin());
                stored.record.database = *database;
                stored.record.url = *url;
                stored.record.mechanism = *decoded_mechanism;
                stored.record.created_at = std::chrono::sys_seconds{std::chrono::seconds{*created}};
                stored.base_url = *base_url;
                stored.base_fingerprint = *fingerprint;
                output.data_branches.push_back(std::move(stored));
            }
        }
        if (const auto* hot_update = find(*config, "hot_update")) {
            const auto* restart_hook_value = find(*hot_update, "restart_hook");
            const auto* targets_value = find(*hot_update, "targets");
            const auto* restart_hook =
                restart_hook_value ? std::get_if<cbor::value::array>(&restart_hook_value->data)
                                   : nullptr;
            const auto* targets =
                targets_value ? std::get_if<cbor::value::array>(&targets_value->data) : nullptr;
            if (!restart_hook || !targets) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid hot-update config", "repository document"));
            }
            for (const auto& argument_value : *restart_hook) {
                const auto* argument = std::get_if<std::string>(&argument_value.data);
                if (!argument || argument->empty()) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid hot-update restart hook", "repository document"));
                }
                output.hot_update.restart_hook.push_back(*argument);
            }
            for (const auto& target_value : *targets) {
                const auto* name_value = find(target_value, "name");
                const auto* endpoint_value = find(target_value, "endpoint");
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                const auto* endpoint =
                    endpoint_value ? std::get_if<std::string>(&endpoint_value->data) : nullptr;
                if (!name || name->empty() || !endpoint || endpoint->empty()) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid hot-update target", "repository document"));
                }
                output.hot_update.targets.push_back({*name, *endpoint});
            }
        }
        if (const auto* deploy = find(*config, "deploy")) {
            const auto* control_value = find(*deploy, "control_plane");
            const auto* environments_value = find(*deploy, "environments");
            const auto* on_push_value = find(*deploy, "on_push");
            const auto* control =
                control_value ? std::get_if<std::string>(&control_value->data) : nullptr;
            const auto* environments =
                environments_value ? std::get_if<cbor::value::array>(&environments_value->data)
                                   : nullptr;
            const auto* on_push =
                on_push_value ? std::get_if<std::string>(&on_push_value->data) : nullptr;
            if (!control || !environments || !on_push ||
                (*on_push != "none" && *on_push != "preview")) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid deployment config", "repository document"));
            }
            output.deploy.control_plane = *control;
            output.deploy.on_push =
                *on_push == "preview" ? deploy_on_push::preview : deploy_on_push::none;
            for (const auto& item : *environments) {
                const auto* name_value = find(item, "name");
                const auto* domain_value = find(item, "domain");
                const auto* env_value = find(item, "env");
                const auto* secrets_value = find(item, "secrets");
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                const auto* variables =
                    env_value ? std::get_if<cbor::value::array>(&env_value->data) : nullptr;
                const auto* secrets =
                    secrets_value ? std::get_if<cbor::value::array>(&secrets_value->data) : nullptr;
                if (!name || name->empty() || !domain_value || !variables || !secrets) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid deployment environment", "repository document"));
                }
                auto environment = deploy_environment_config{};
                if (!std::holds_alternative<cbor::null>(domain_value->data)) {
                    const auto* domain = std::get_if<std::string>(&domain_value->data);
                    if (!domain || domain->empty()) {
                        return std::unexpected(failure(
                            errc::corrupt, "invalid deployment domain", "repository document"));
                    }
                    environment.domain = *domain;
                }
                for (const auto& variable : *variables) {
                    const auto* key_value = find(variable, "name");
                    const auto* value_value = find(variable, "value");
                    const auto* key =
                        key_value ? std::get_if<std::string>(&key_value->data) : nullptr;
                    const auto* value =
                        value_value ? std::get_if<std::string>(&value_value->data) : nullptr;
                    if (!key || key->empty() || !value ||
                        !environment.env.emplace(*key, *value).second) {
                        return std::unexpected(failure(
                            errc::corrupt, "invalid deployment variable", "repository document"));
                    }
                }
                for (const auto& secret_value : *secrets) {
                    const auto* secret = std::get_if<std::string>(&secret_value.data);
                    if (!secret || secret->empty()) {
                        return std::unexpected(failure(errc::corrupt,
                                                       "invalid deployment secret reference",
                                                       "repository document"));
                    }
                    environment.secrets.push_back(*secret);
                }
                if (!output.deploy.environments.emplace(*name, std::move(environment)).second) {
                    return std::unexpected(failure(
                        errc::corrupt, "duplicate deployment environment", "repository document"));
                }
            }
        }
        if (const auto* flow = find(*config, "flow")) {
            const auto decode_bucket = [&](std::string_view name) -> result<token_bucket_config> {
                const auto* encoded = find(*flow, name);
                const auto* rate_value = encoded ? find(*encoded, "rate_per_s") : nullptr;
                const auto* burst_value = encoded ? find(*encoded, "burst") : nullptr;
                const auto* rate =
                    rate_value ? std::get_if<std::uint64_t>(&rate_value->data) : nullptr;
                const auto* burst =
                    burst_value ? std::get_if<std::uint64_t>(&burst_value->data) : nullptr;
                if (!rate || !burst) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid flow bucket", "repository document"));
                }
                return token_bucket_config{*rate, *burst};
            };
            auto per_actor = decode_bucket("per_actor");
            auto per_document = decode_bucket("per_document");
            auto per_repository = decode_bucket("per_repository");
            const auto* overrides_value = find(*flow, "actor_overrides");
            const auto* max_update_value = find(*flow, "max_update_bytes");
            const auto* max_unacknowledged_value = find(*flow, "max_unacknowledged");
            const auto* pause_value = find(*flow, "pause_above");
            const auto* resume_value = find(*flow, "resume_below");
            const auto* overrides =
                overrides_value ? std::get_if<cbor::value::array>(&overrides_value->data) : nullptr;
            const auto* max_update =
                max_update_value ? std::get_if<std::uint64_t>(&max_update_value->data) : nullptr;
            const auto* max_unacknowledged =
                max_unacknowledged_value
                    ? std::get_if<std::uint64_t>(&max_unacknowledged_value->data)
                    : nullptr;
            const auto* pause =
                pause_value ? std::get_if<std::uint64_t>(&pause_value->data) : nullptr;
            const auto* resume =
                resume_value ? std::get_if<std::uint64_t>(&resume_value->data) : nullptr;
            if (!per_actor || !per_document || !per_repository || !overrides || !max_update ||
                !max_unacknowledged || !pause || !resume) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid flow config", "repository document"));
            }
            output.flow = {*per_actor,
                           *per_document,
                           *per_repository,
                           {},
                           *max_update,
                           *max_unacknowledged,
                           *pause,
                           *resume};
            for (const auto& item : *overrides) {
                const auto* id_value = find(item, "taap_id");
                const auto* bucket_value = find(item, "bucket");
                const auto* id = id_value ? std::get_if<std::string>(&id_value->data) : nullptr;
                const auto* rate_value = bucket_value ? find(*bucket_value, "rate_per_s") : nullptr;
                const auto* burst_value = bucket_value ? find(*bucket_value, "burst") : nullptr;
                const auto* rate =
                    rate_value ? std::get_if<std::uint64_t>(&rate_value->data) : nullptr;
                const auto* burst =
                    burst_value ? std::get_if<std::uint64_t>(&burst_value->data) : nullptr;
                if (!id || !rate || !burst ||
                    !output.flow.actor_overrides.emplace(*id, token_bucket_config{*rate, *burst})
                         .second) {
                    return std::unexpected(failure(
                        errc::corrupt, "invalid flow actor override", "repository document"));
                }
            }
        }
        if (const auto* remote = find(*config, "remote");
            remote && !std::holds_alternative<cbor::null>(remote->data)) {
            const auto* endpoint_value = find(*remote, "endpoint");
            const auto* instance_value = find(*remote, "instance_name");
            const auto* backend_value = find(*remote, "backend");
            const auto* tls_value = find(*remote, "tls");
            const auto* headers_value = find(*remote, "headers");
            const auto* deadline_value = find(*remote, "deadline_ms");
            const auto* limit_value = find(*remote, "batch_blob_limit");
            const auto* publish_value = find(*remote, "publish");
            const auto* endpoint =
                endpoint_value ? std::get_if<std::string>(&endpoint_value->data) : nullptr;
            const auto* instance =
                instance_value ? std::get_if<std::string>(&instance_value->data) : nullptr;
            const auto* backend =
                backend_value ? std::get_if<std::uint64_t>(&backend_value->data) : nullptr;
            const auto* headers =
                headers_value ? std::get_if<cbor::value::array>(&headers_value->data) : nullptr;
            const auto* deadline =
                deadline_value ? std::get_if<std::uint64_t>(&deadline_value->data) : nullptr;
            const auto* limit =
                limit_value ? std::get_if<std::uint64_t>(&limit_value->data) : nullptr;
            const auto* publish = publish_value ? std::get_if<bool>(&publish_value->data) : nullptr;
            if (!endpoint || endpoint->empty() || !instance ||
                (backend &&
                 *backend > static_cast<std::uint64_t>(remote_backend::liquid_scheduler)) ||
                !tls_value || !headers || !deadline || *deadline == 0 ||
                *deadline > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                !limit || *limit == 0 || !publish) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid remote config", "repository document"));
            }
            auto configured = remote_config{
                .endpoint = *endpoint,
                .instance_name = *instance,
                .deadline = std::chrono::milliseconds{*deadline},
                .batch_blob_limit = *limit,
                .publish = *publish,
                .backend = backend ? static_cast<remote_backend>(*backend) : remote_backend::reapi};
            const auto* tls_enabled_value = find(*tls_value, "enabled");
            const auto* verify_peer_value = find(*tls_value, "verify_peer");
            const auto* tls_enabled =
                tls_enabled_value ? std::get_if<bool>(&tls_enabled_value->data) : nullptr;
            const auto* verify_peer =
                verify_peer_value ? std::get_if<bool>(&verify_peer_value->data) : nullptr;
            if (!tls_enabled || !verify_peer) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid remote TLS config", "repository document"));
            }
            configured.tls.enabled = *tls_enabled;
            configured.tls.verify_peer = *verify_peer;
            const auto read_path =
                [&](std::string_view name) -> result<std::optional<std::filesystem::path>> {
                const auto* field = find(*tls_value, name);
                if (!field || std::holds_alternative<cbor::null>(field->data)) {
                    return std::optional<std::filesystem::path>{};
                }
                const auto* text = std::get_if<std::string>(&field->data);
                if (!text || text->empty()) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid remote TLS path", "repository document"));
                }
                return std::filesystem::path{*text};
            };
            auto authority = read_path("certificate_authority");
            auto certificate = read_path("client_certificate");
            auto key = read_path("client_key");
            if (!authority || !certificate || !key) {
                return std::unexpected(!authority     ? authority.error()
                                       : !certificate ? certificate.error()
                                                      : key.error());
            }
            configured.tls.certificate_authority = std::move(*authority);
            configured.tls.client_certificate = std::move(*certificate);
            configured.tls.client_key = std::move(*key);
            for (const auto& header : *headers) {
                const auto* name_value = find(header, "name");
                const auto* value_value = find(header, "value");
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                const auto* value =
                    value_value ? std::get_if<std::string>(&value_value->data) : nullptr;
                if (!name || name->empty() || !value) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid remote header", "repository document"));
                }
                configured.headers.insert_or_assign(*name, *value);
            }
            output.remote = std::move(configured);
        }
        if (const auto* policy = find(*config, "policy")) {
            const auto* approved = find(*policy, "approved_builders");
            const auto* rules = find(*policy, "rules");
            const auto* approved_values =
                approved ? std::get_if<cbor::value::array>(&approved->data) : nullptr;
            const auto* rule_values =
                rules ? std::get_if<cbor::value::array>(&rules->data) : nullptr;
            if (!approved_values || !rule_values) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid input policy config", "repository document"));
            }
            for (const auto& item : *approved_values) {
                const auto* text = std::get_if<std::string>(&item.data);
                if (!text) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid approved builder", "repository document"));
                }
                output.policy.approved_builders.push_back(*text);
            }
            for (const auto& item : *rule_values) {
                const auto* scope_value = find(item, "scope");
                const auto* paths = find(item, "paths");
                const auto* requirements = find(item, "require");
                const auto* on_missing_value = find(item, "on_missing");
                const auto* scope =
                    scope_value ? std::get_if<std::string>(&scope_value->data) : nullptr;
                const auto* path_values =
                    paths ? std::get_if<cbor::value::array>(&paths->data) : nullptr;
                const auto* requirement_values =
                    requirements ? std::get_if<cbor::value::array>(&requirements->data) : nullptr;
                const auto* on_missing =
                    on_missing_value ? std::get_if<std::string>(&on_missing_value->data) : nullptr;
                if (!scope || !path_values || !requirement_values || !on_missing ||
                    (*scope != "paths" && *scope != "toolchain" && *scope != "all inputs") ||
                    (*on_missing != "refuse" && *on_missing != "warn")) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid input policy rule", "repository document"));
                }
                auto rule =
                    policy_rule{.scope = *scope == "paths"       ? policy_scope::paths
                                         : *scope == "toolchain" ? policy_scope::toolchain
                                                                 : policy_scope::all_inputs,
                                .on_missing = *on_missing == "refuse" ? policy_on_missing::refuse
                                                                      : policy_on_missing::warn};
                for (const auto& path : *path_values) {
                    const auto* text = std::get_if<std::string>(&path.data);
                    if (!text) {
                        return std::unexpected(
                            failure(errc::corrupt, "invalid policy path", "repository document"));
                    }
                    rule.paths.push_back(*text);
                }
                for (const auto& required : *requirement_values) {
                    const auto* profile_value = find(required, "profile");
                    const auto* from_value = find(required, "from");
                    const auto* count_value = find(required, "min_count");
                    const auto* profile =
                        profile_value ? std::get_if<std::string>(&profile_value->data) : nullptr;
                    const auto* from =
                        from_value ? std::get_if<cbor::value::array>(&from_value->data) : nullptr;
                    const auto* count =
                        count_value ? std::get_if<std::uint64_t>(&count_value->data) : nullptr;
                    if (!profile || !from || !count || *count == 0 ||
                        *count > std::numeric_limits<std::uint32_t>::max()) {
                        return std::unexpected(failure(
                            errc::corrupt, "invalid policy requirement", "repository document"));
                    }
                    auto requirement = policy_requirement{
                        .profile = *profile, .min_count = static_cast<std::uint32_t>(*count)};
                    for (const auto& authority : *from) {
                        const auto* text = std::get_if<std::string>(&authority.data);
                        if (!text) {
                            return std::unexpected(failure(
                                errc::corrupt, "invalid policy authority", "repository document"));
                        }
                        requirement.from.push_back(*text);
                    }
                    rule.require.push_back(std::move(requirement));
                }
                output.policy.rules.push_back(std::move(rule));
            }
        }
        if (const auto* retention = find(*config, "retention")) {
            const auto* rules_value = find(*retention, "rules");
            const auto* rules =
                rules_value ? std::get_if<cbor::value::array>(&rules_value->data) : nullptr;
            if (!rules) {
                return std::unexpected(
                    failure(errc::corrupt, "invalid retention config", "repository document"));
            }
            output.retention.rules.clear();
            for (const auto& item : *rules) {
                const auto* type_value = find(item, "type");
                const auto* name_value = find(item, "name");
                const auto* type =
                    type_value ? std::get_if<std::string>(&type_value->data) : nullptr;
                const auto* name =
                    name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
                if (!type || !name) {
                    return std::unexpected(
                        failure(errc::corrupt, "invalid retention rule", "repository document"));
                }
                if (*type == "refs") {
                    const auto* glob_value = find(item, "glob");
                    const auto* duration_value = find(item, "duration_s");
                    const auto* forever_value = find(item, "forever");
                    const auto* glob =
                        glob_value ? std::get_if<std::string>(&glob_value->data) : nullptr;
                    const auto* duration = duration_value
                                               ? std::get_if<std::uint64_t>(&duration_value->data)
                                               : nullptr;
                    const auto* forever =
                        forever_value ? std::get_if<bool>(&forever_value->data) : nullptr;
                    if (!glob || ((duration != nullptr) == (forever && *forever)) ||
                        (forever && !*forever) ||
                        (duration && (*duration == 0 ||
                                      *duration > static_cast<std::uint64_t>(
                                                      std::numeric_limits<std::int64_t>::max())))) {
                        return std::unexpected(failure(
                            errc::corrupt, "invalid ref retention rule", "repository document"));
                    }
                    auto configured_duration = std::optional<std::chrono::seconds>{};
                    if (duration) {
                        configured_duration =
                            std::chrono::seconds{static_cast<std::int64_t>(*duration)};
                    }
                    output.retention.rules.push_back(
                        ref_retention_rule{*name, *glob, configured_duration});
                } else if (*type == "results") {
                    const auto* duration_value = find(item, "duration_s");
                    const auto* duration = duration_value
                                               ? std::get_if<std::uint64_t>(&duration_value->data)
                                               : nullptr;
                    if (!duration || *duration == 0 ||
                        *duration >
                            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                        return std::unexpected(failure(
                            errc::corrupt, "invalid result retention rule", "repository document"));
                    }
                    output.retention.rules.push_back(result_retention_rule{
                        *name, std::chrono::seconds{static_cast<std::int64_t>(*duration)}});
                } else if (*type == "size_cap") {
                    const auto* bytes_value = find(item, "bytes");
                    const auto* byte_count =
                        bytes_value ? std::get_if<std::uint64_t>(&bytes_value->data) : nullptr;
                    if (!byte_count || *byte_count == 0) {
                        return std::unexpected(failure(errc::corrupt,
                                                       "invalid size-cap retention rule",
                                                       "repository document"));
                    }
                    output.retention.rules.push_back(size_cap_retention_rule{*name, *byte_count});
                } else {
                    return std::unexpected(failure(
                        errc::corrupt, "unknown retention rule type", "repository document"));
                }
            }
        }
    }
    for (const auto& item : *saved_drips) {
        const auto* author = find(item, "author");
        const auto* body = find(item, "body");
        const auto* signature = find(item, "sig");
        const auto* body_bytes = body ? std::get_if<cbor::value::bytes>(&body->data) : nullptr;
        const auto* signature_bytes =
            signature ? std::get_if<cbor::value::bytes>(&signature->data) : nullptr;
        if (!body_bytes || !signature_bytes) {
            return std::unexpected(
                failure(errc::corrupt, "invalid drip record", "repository document"));
        }
        auto decoded_author = author ? decode_actor(*author) : result<actor>{actor{}};
        if (!decoded_author) {
            return std::unexpected(decoded_author.error());
        }
        output.drips.push_back({std::move(*decoded_author), *body_bytes, *signature_bytes});
    }
    return output;
}
auto random_id() -> node_id {
    auto result = node_id{};
    auto random = std::random_device{};
    std::ranges::generate(result, [&] { return std::byte{static_cast<unsigned char>(random())}; });
    return result;
}
auto random_operation_id(algorithm hash_algorithm) -> result<op_id> {
    auto value = random_id();
    auto hashed = hash(hash_algorithm, std::span<const std::byte>{value});
    if (!hashed) {
        return std::unexpected(hashed.error());
    }
    return op_id{*hashed};
}
auto split_path(std::string_view path) -> result<std::vector<std::string>> {
    if (path.empty() || path.front() == '/' || path.back() == '/')
        return std::unexpected(failure(
            errc::invalid_argument, "path must be relative and non-empty", std::string{path}));
    std::vector<std::string> result;
    for (std::size_t start = 0; start < path.size();) {
        const auto end = path.find('/', start);
        const auto part =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..")
            return std::unexpected(
                failure(errc::invalid_argument, "invalid path component", std::string{path}));
        if (const auto problem = objects::entry_name_problem(part))
            return std::unexpected(failure(errc::invalid_argument,
                                           std::string{*problem},
                                           objects::printable_entry_name(path)));
        result.emplace_back(part);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}
auto parent_for(working_entry& root,
                const std::vector<std::string>& path,
                bool create,
                algorithm hash_algorithm) -> result<working_entry*> {
    working_entry* current = &root;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        auto found = current->entries.find(path[i]);
        if (found == current->entries.end()) {
            if (!create)
                return std::unexpected(
                    failure(errc::not_found, "directory does not exist", path[i]));
            found =
                current->entries
                    .emplace(
                        path[i],
                        working_entry{
                            entry_kind::directory, random_id(), 0040755, {hash_algorithm, {}}, {}})
                    .first;
        }
        if (found->second.kind != entry_kind::directory)
            return std::unexpected(
                failure(errc::invalid_argument, "path component is not a directory", path[i]));
        current = &found->second;
    }
    return current;
}
auto lookup(working_entry& root, const std::vector<std::string>& path) -> working_entry* {
    working_entry* current = &root;
    for (const auto& part : path) {
        auto found = current->entries.find(part);
        if (found == current->entries.end())
            return nullptr;
        current = &found->second;
    }
    return current;
}
auto lookup(const working_entry& root, const std::vector<std::string>& path)
    -> const working_entry* {
    const working_entry* current = &root;
    for (const auto& part : path) {
        const auto found = current->entries.find(part);
        if (found == current->entries.end()) {
            return nullptr;
        }
        current = &found->second;
    }
    return current;
}
auto directory_entry(const working_entry& value) -> crdt::entry {
    const auto kind = value.kind == entry_kind::file        ? "blob"
                      : value.kind == entry_kind::directory ? "tree"
                                                            : "repo";
    return {kind, value.node, value.mode, to_string(value.object)};
}
auto directory_with_node(working_entry& root, const node_id& node) -> working_entry* {
    if (root.node == node) {
        return &root;
    }
    for (auto& [name, child] : root.entries) {
        if (child.kind != entry_kind::file) {
            if (auto* found = directory_with_node(child, node)) {
                return found;
            }
        }
    }
    return nullptr;
}

struct sync_cursor_position {
    std::size_t drips{};
    std::size_t items{};
};

auto parse_sync_cursor(std::string_view token) -> result<sync_cursor_position> {
    if (token.empty()) {
        return sync_cursor_position{};
    }
    const auto separator = token.find(':');
    const auto drip_text = token.substr(0, separator);
    const auto item_text =
        separator == std::string_view::npos ? std::string_view{} : token.substr(separator + 1);
    auto output = sync_cursor_position{};
    const auto drip_result =
        std::from_chars(drip_text.data(), drip_text.data() + drip_text.size(), output.drips);
    if (drip_result.ec != std::errc{} || drip_result.ptr != drip_text.data() + drip_text.size()) {
        return std::unexpected(
            failure(errc::invalid_argument, "invalid sync cursor", std::string{token}));
    }
    if (!item_text.empty()) {
        const auto item_result =
            std::from_chars(item_text.data(), item_text.data() + item_text.size(), output.items);
        if (item_result.ec != std::errc{} ||
            item_result.ptr != item_text.data() + item_text.size()) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid sync cursor", std::string{token}));
        }
    }
    return output;
}

auto sync_item_failure(std::size_t item, error source) -> error {
    source.context = "item=" + std::to_string(item) +
                     (source.context.empty() ? std::string{} : ": " + source.context);
    return source;
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto same_change(const std::optional<change_id>& left, const std::optional<change_id>& right)
    -> bool {
    if (!left || !right) {
        return !left && !right;
    }
    return same_digest(left->value, right->value);
}

auto same_nested(const std::optional<repo_ref>& left, const std::optional<repo_ref>& right)
    -> bool {
    if (!left || !right) {
        return !left && !right;
    }
    return left->repo_id == right->repo_id && same_digest(left->id.value, right->id.value);
}

auto same_entry(const working_entry& left, const working_entry& right) -> bool {
    if (left.kind != right.kind || left.node != right.node || left.mode != right.mode ||
        !same_digest(left.object, right.object) || !same_nested(left.nested, right.nested) ||
        left.entries.size() != right.entries.size()) {
        return false;
    }
    for (const auto& [name, child] : left.entries) {
        const auto found = right.entries.find(name);
        if (found == right.entries.end() || !same_entry(child, found->second)) {
            return false;
        }
    }
    return true;
}

auto same_entry(const working_entry* current, const std::optional<working_entry>& expected)
    -> bool {
    if (!current || !expected) {
        return !current && !expected;
    }
    return same_entry(*current, *expected);
}

auto operation_matches(const op_id& left, const op_id& right) -> bool {
    return same_digest(left.value, right.value);
}

void append_nested_pointers(const working_entry& entry,
                            std::string path,
                            std::vector<document_nested_pointer>& output) {
    for (const auto& [name, child] : entry.entries) {
        const auto child_path = path.empty() ? name : path + "/" + name;
        if (child.kind == entry_kind::nested_repository && child.nested) {
            output.push_back({child_path, *child.nested});
        }
        append_nested_pointers(child, child_path, output);
    }
}

auto public_document_state(const repository_document& source) -> document_state {
    auto output = document_state{};
    output.refs = source.refs;
    for (const auto& lock : source.locks) {
        output.locks.push_back(lock.path);
    }
    append_nested_pointers(source.root, {}, output.nested);
    return output;
}

auto same_path_or_descendant(std::string_view path, std::string_view root) -> bool;

auto selected_path(std::string_view path, const undo_options& options) -> bool {
    return options.paths.empty() || std::ranges::any_of(options.paths, [&](const auto& selected) {
               return same_path_or_descendant(path, selected);
           });
}

auto selected_drip(const op_id& id, const undo_options& options) -> bool {
    return options.drips.empty() || std::ranges::any_of(options.drips, [&](const auto& selected) {
               return operation_matches(id, selected);
           });
}

auto replace_entry(working_entry& root,
                   std::string_view path,
                   const std::optional<working_entry>& replacement,
                   algorithm hash_algorithm) -> result<void> {
    auto parts = split_path(path);
    if (!parts) {
        return std::unexpected(parts.error());
    }
    auto parent = parent_for(root, *parts, replacement.has_value(), hash_algorithm);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    if (replacement) {
        (*parent)->entries[parts->back()] = *replacement;
    } else {
        (*parent)->entries.erase(parts->back());
    }
    return {};
}

auto optional_ref(const std::map<std::string, change_id>& refs, std::string_view name)
    -> std::optional<change_id> {
    const auto found = refs.find(std::string{name});
    if (found == refs.end()) {
        return std::nullopt;
    }
    return found->second;
}

void replace_ref(std::map<std::string, change_id>& refs,
                 const std::string& name,
                 const std::optional<change_id>& replacement) {
    if (replacement) {
        refs[name] = *replacement;
    } else {
        refs.erase(name);
    }
}

struct planned_undo {
    repository_document document;
    undo_report report;
    std::set<std::string> changed_paths;
};

auto append_inverse_skip(undo_report& report,
                         const stored_operation& operation,
                         std::string path,
                         std::string reason) -> void {
    report.skipped.push_back({operation.id, std::move(path), std::move(reason)});
}

auto plan_entry_inverses(repository_document& candidate,
                         const stored_operation& source,
                         const undo_options& options,
                         stored_operation& inverse,
                         undo_report& report,
                         std::set<std::string>& changed_paths,
                         algorithm hash_algorithm) -> result<bool> {
    auto applied = false;
    const auto whole_operation = source.kind == "move" && !options.paths.empty() &&
                                 std::ranges::any_of(source.entries, [&](const auto& effect) {
                                     return selected_path(effect.path, options);
                                 });
    for (const auto& effect : source.entries) {
        if (!whole_operation && !selected_path(effect.path, options)) {
            continue;
        }
        auto parts = split_path(effect.path);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        const auto* current = lookup(candidate.root, *parts);
        if (!same_entry(current, effect.after)) {
            append_inverse_skip(
                report, source, effect.path, "current state no longer reflects the operation");
            continue;
        }
        if (source.kind == "redact") {
            append_inverse_skip(report, source, effect.path, "redacted content is unavailable");
            continue;
        }
        auto replaced = replace_entry(candidate.root, effect.path, effect.before, hash_algorithm);
        if (!replaced) {
            return std::unexpected(replaced.error());
        }
        inverse.entries.push_back({effect.path, effect.after, effect.before});
        changed_paths.insert(effect.path);
        applied = true;
    }
    return applied;
}

auto plan_ref_inverses(repository_document& candidate,
                       const stored_operation& source,
                       const undo_options& options,
                       stored_operation& inverse,
                       undo_report& report) -> bool {
    if (!options.paths.empty()) {
        return false;
    }
    auto applied = false;
    for (const auto& effect : source.refs) {
        const auto current = optional_ref(candidate.refs, effect.name);
        if (!same_change(current, effect.after)) {
            append_inverse_skip(report,
                                source,
                                "ref:" + effect.name,
                                "current reference no longer reflects the operation");
            continue;
        }
        replace_ref(candidate.refs, effect.name, effect.before);
        inverse.refs.push_back({effect.name, effect.after, effect.before});
        applied = true;
    }
    if (source.head_after && same_change(candidate.head, source.head_after)) {
        candidate.head = source.head_before;
        inverse.head_before = source.head_after;
        inverse.head_after = source.head_before;
        applied = true;
    } else if (source.head_after) {
        append_inverse_skip(
            report, source, "head", "current head no longer reflects the operation");
    }
    return applied;
}

auto plan_lock_inverses(repository_document& candidate,
                        const stored_operation& source,
                        const undo_options& options,
                        stored_operation& inverse,
                        undo_report& report) -> bool {
    auto applied = false;
    for (const auto& effect : source.locks) {
        if (!selected_path(effect.path, options)) {
            continue;
        }
        const auto existing =
            std::ranges::find(candidate.locks, effect.path, &exclusive_lock::path);
        const auto current = existing != candidate.locks.end();
        if (current != effect.after) {
            append_inverse_skip(
                report, source, effect.path, "current lock no longer reflects the operation");
            continue;
        }
        if (effect.before) {
            if (!current) {
                candidate.locks.push_back({.path = effect.path,
                                           .holder = inverse.by,
                                           .since = inverse.at,
                                           .reason = "restored by undo",
                                           .authority = lock_authority::advisory});
            }
        } else {
            std::erase_if(candidate.locks,
                          [&](const auto& value) { return value.path == effect.path; });
        }
        inverse.locks.push_back({effect.path, effect.after, effect.before});
        applied = true;
    }
    return applied;
}

auto plan_undo(repository_document current,
               std::span<const stored_operation> operations,
               const undo_options& options,
               algorithm hash_algorithm,
               session_id session,
               const actor& actor) -> result<planned_undo> {
    auto output = planned_undo{.document = std::move(current)};
    for (const auto& source : operations) {
        if (!selected_drip(source.id, options)) {
            continue;
        }
        auto inverse = stored_operation{};
        auto inverse_id = random_operation_id(hash_algorithm);
        if (!inverse_id) {
            return std::unexpected(inverse_id.error());
        }
        inverse.id = *inverse_id;
        inverse.kind = "undo";
        inverse.from = to_string(source.id.value);
        inverse.session = session;
        inverse.by = actor;
        inverse.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto entries = plan_entry_inverses(output.document,
                                           source,
                                           options,
                                           inverse,
                                           output.report,
                                           output.changed_paths,
                                           hash_algorithm);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        const auto refs =
            plan_ref_inverses(output.document, source, options, inverse, output.report);
        const auto locks =
            plan_lock_inverses(output.document, source, options, inverse, output.report);
        if (*entries || refs || locks) {
            output.report.applied.push_back(source.id);
            output.document.operations.push_back(std::move(inverse));
        }
    }
    return output;
}

struct decoded_drip_body {
    std::optional<change_id> base;
    std::vector<std::string> paths;
    std::optional<write_base> reasoned_from;
    std::optional<stale_revalidation> revalidated;
    std::optional<digest> delegation;
    std::optional<actor> agent;
    std::optional<exclusive_lock> lock;
    std::optional<lock_config> lock_rules;
    std::string operation;
    std::int64_t at{};
};

auto decode_repository_drip(std::span<const std::byte> input) -> result<decoded_drip_body> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = find(*decoded, "v");
    const auto* kind_value = find(*decoded, "kind");
    const auto* operation_value = find(*decoded, "operation");
    const auto* at_value = find(*decoded, "at");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* operation =
        operation_value ? std::get_if<std::string>(&operation_value->data) : nullptr;
    if (!version || (*version != 1 && *version != 2 && *version != 3 && *version != 4) || !kind ||
        !at_value) {
        return std::unexpected(failure(errc::corrupt, "invalid signed drip body"));
    }
    auto output = decoded_drip_body{};
    if (const auto* at = std::get_if<std::int64_t>(&at_value->data)) {
        output.at = *at;
    } else if (const auto* at = std::get_if<std::uint64_t>(&at_value->data);
               at && *at <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        output.at = static_cast<std::int64_t>(*at);
    } else {
        return std::unexpected(failure(errc::corrupt, "invalid signed drip timestamp"));
    }
    if (*version == 1 && *kind == "redaction") {
        const auto* path_value = find(*decoded, "path");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        if (!path) {
            return std::unexpected(failure(errc::corrupt, "invalid redaction drip body"));
        }
        output.operation = "redaction";
        output.paths.push_back(*path);
        return output;
    }
    if (*kind != "drip" || !operation) {
        return std::unexpected(failure(errc::corrupt, "invalid signed drip operation"));
    }
    output.operation = *operation;
    if (*version == 1) {
        return output;
    }
    const auto* base_value = find(*decoded, "base");
    const auto* paths_value = find(*decoded, "paths");
    const auto* paths = paths_value ? std::get_if<cbor::value::array>(&paths_value->data) : nullptr;
    if (!base_value || !paths) {
        return std::unexpected(failure(errc::corrupt, "invalid version 2 drip body"));
    }
    if (!std::holds_alternative<cbor::null>(base_value->data)) {
        auto base = decode_digest(*base_value);
        if (!base) {
            return std::unexpected(base.error());
        }
        output.base = change_id{*base};
    }
    for (const auto& item : *paths) {
        const auto* path = std::get_if<std::string>(&item.data);
        if (!path) {
            return std::unexpected(failure(errc::corrupt, "invalid drip path"));
        }
        output.paths.push_back(*path);
    }
    if (const auto* reasoned_value = find(*decoded, "reasoned_from")) {
        const auto* change_value = find(*reasoned_value, "change");
        const auto* cursor_value = find(*reasoned_value, "cursor");
        if (!change_value || !cursor_value) {
            return std::unexpected(failure(errc::corrupt, "invalid reasoned-from base"));
        }
        auto change = decode_digest(*change_value);
        if (!change) {
            return std::unexpected(change.error());
        }
        auto base = write_base{.change = change_id{*change}};
        if (!std::holds_alternative<cbor::null>(cursor_value->data)) {
            const auto* cursor = std::get_if<std::string>(&cursor_value->data);
            if (!cursor) {
                return std::unexpected(failure(errc::corrupt, "invalid reasoned-from cursor"));
            }
            base.cursor = journal_cursor{*cursor};
        }
        output.reasoned_from = std::move(base);
    }
    if (const auto* revalidated_value = find(*decoded, "revalidated")) {
        const auto* revalidated = std::get_if<std::string>(&revalidated_value->data);
        if (!revalidated || (*revalidated != "disjoint" && *revalidated != "forced")) {
            return std::unexpected(failure(errc::corrupt, "invalid drip revalidation"));
        }
        output.revalidated =
            *revalidated == "disjoint" ? stale_revalidation::disjoint : stale_revalidation::forced;
    }
    if (*version == 3) {
        const auto* delegation_value = find(*decoded, "delegation");
        const auto* agent_value = find(*decoded, "agent");
        if (!delegation_value || !agent_value) {
            return std::unexpected(failure(errc::corrupt, "invalid version 3 drip body"));
        }
        auto delegation = decode_digest(*delegation_value);
        auto agent = decode_actor(*agent_value);
        if (!delegation || !agent) {
            return std::unexpected(delegation ? agent.error() : delegation.error());
        }
        output.delegation = *delegation;
        output.agent = std::move(*agent);
    }
    if (*version == 4) {
        if (const auto* lock_encoded = find(*decoded, "lock");
            lock_encoded && !std::holds_alternative<cbor::null>(lock_encoded->data)) {
            auto lock = decode_lock(*lock_encoded);
            if (!lock) {
                return std::unexpected(lock.error());
            }
            output.lock = std::move(*lock);
        }
        if (const auto* rules_encoded = find(*decoded, "lock_rules");
            rules_encoded && !std::holds_alternative<cbor::null>(rules_encoded->data)) {
            auto rules = decode_lock_config(*rules_encoded);
            if (!rules) {
                return std::unexpected(rules.error());
            }
            output.lock_rules = std::move(*rules);
        }
        const auto valid_lock_operation =
            (output.operation == "lock" || output.operation == "unlock") && output.lock;
        const auto valid_config_operation = output.operation == "lock_config" && output.lock_rules;
        if (!valid_lock_operation && !valid_config_operation) {
            return std::unexpected(failure(errc::corrupt, "invalid version 4 drip body"));
        }
    }
    return output;
}

auto entry_parent(const working_entry& root, const std::vector<std::string>& path)
    -> const working_entry* {
    const working_entry* current = &root;
    for (std::size_t index = 0; index + 1 < path.size(); ++index) {
        const auto found = current->entries.find(path[index]);
        if (found == current->entries.end()) {
            return nullptr;
        }
        current = &found->second;
    }
    return current;
}

auto crdt_inverse_operations(const working_entry& before,
                             const working_entry& after,
                             const std::set<std::string>& paths)
    -> result<std::map<node_id, std::vector<crdt::operation>>> {
    auto output = std::map<node_id, std::vector<crdt::operation>>{};
    for (const auto& path : paths) {
        auto parts = split_path(path);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        const auto* before_parent = entry_parent(before, *parts);
        const auto* after_parent = entry_parent(after, *parts);
        if (!before_parent && !after_parent) {
            return std::unexpected(failure(errc::corrupt, "undo path parent is absent", path));
        }
        const auto* before_entry = lookup(before, *parts);
        const auto* after_entry = lookup(after, *parts);
        if (before_entry && after_entry && same_entry(*before_entry, *after_entry)) {
            continue;
        }
        const auto parent_node = after_parent ? after_parent->node : before_parent->node;
        auto& operations = output[parent_node];
        if (after_entry) {
            const auto kind = after_entry->kind == entry_kind::file        ? "blob"
                              : after_entry->kind == entry_kind::directory ? "tree"
                                                                           : "repo";
            operations.push_back({.kind = crdt::operation_kind::write,
                                  .path = parts->back(),
                                  .ref = to_string(after_entry->object),
                                  .mode = after_entry->mode,
                                  .entry_id = after_entry->node});
        } else if (before_entry) {
            operations.push_back({.kind = crdt::operation_kind::remove, .path = parts->back()});
        }
    }
    return output;
}

void append_entry_effects(const working_entry& before,
                          const working_entry& after,
                          std::string prefix,
                          std::vector<stored_entry_effect>& output) {
    auto names = std::set<std::string>{};
    for (const auto& [name, entry] : before.entries) {
        static_cast<void>(entry);
        names.insert(name);
    }
    for (const auto& [name, entry] : after.entries) {
        static_cast<void>(entry);
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto old = before.entries.find(name);
        const auto replacement = after.entries.find(name);
        const auto path = prefix.empty() ? name : prefix + "/" + name;
        if (old == before.entries.end()) {
            output.push_back({path, std::nullopt, replacement->second});
            continue;
        }
        if (replacement == after.entries.end()) {
            output.push_back({path, old->second, std::nullopt});
            continue;
        }
        if (old->second.kind == entry_kind::directory &&
            replacement->second.kind == entry_kind::directory &&
            old->second.node == replacement->second.node) {
            append_entry_effects(old->second, replacement->second, path, output);
            continue;
        }
        if (!same_entry(old->second, replacement->second)) {
            output.push_back({path, old->second, replacement->second});
        }
    }
}

auto merge_operation(const repository_document& before,
                     const repository_document& after,
                     algorithm hash_algorithm,
                     session_id session,
                     const actor& actor,
                     std::size_t item_count,
                     std::map<std::string, std::string> metadata = {}) -> result<stored_operation> {
    auto id = random_operation_id(hash_algorithm);
    if (!id) {
        return std::unexpected(id.error());
    }
    auto output = stored_operation{};
    output.id = *id;
    output.kind = "merge";
    output.from = "0";
    output.to = std::to_string(item_count);
    output.session = session;
    output.by = actor;
    output.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    output.metadata = std::move(metadata);
    append_entry_effects(before.root, after.root, {}, output.entries);
    auto ref_names = std::set<std::string>{};
    for (const auto& [name, id_value] : before.refs) {
        static_cast<void>(id_value);
        ref_names.insert(name);
    }
    for (const auto& [name, id_value] : after.refs) {
        static_cast<void>(id_value);
        ref_names.insert(name);
    }
    for (const auto& name : ref_names) {
        const auto old = optional_ref(before.refs, name);
        const auto replacement = optional_ref(after.refs, name);
        if (!same_change(old, replacement)) {
            output.refs.push_back({name, old, replacement});
        }
    }
    auto lock_names = std::set<std::string>{};
    for (const auto& lock : before.locks) {
        lock_names.insert(lock.path);
    }
    for (const auto& lock : after.locks) {
        lock_names.insert(lock.path);
    }
    for (const auto& path : lock_names) {
        const auto old =
            std::ranges::find(before.locks, path, &exclusive_lock::path) != before.locks.end();
        const auto replacement =
            std::ranges::find(after.locks, path, &exclusive_lock::path) != after.locks.end();
        if (old != replacement) {
            output.locks.push_back({path, old, replacement});
        }
    }
    if (!same_change(before.head, after.head)) {
        output.head_before = before.head;
        output.head_after = after.head;
    }
    return output;
}

auto paths_overlap(std::string_view left, std::string_view right) -> bool {
    const auto descendant = [](std::string_view path, std::string_view root) {
        return path == root ||
               (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
    };
    return descendant(left, right) || descendant(right, left);
}

void append_working_roots(const working_entry& entry, std::vector<digest>& output) {
    if (entry.kind == entry_kind::file || entry.kind == entry_kind::nested_repository) {
        output.push_back(entry.object);
    }
    for (const auto& [name, child] : entry.entries) {
        static_cast<void>(name);
        append_working_roots(child, output);
    }
}

void append_working_directories(const working_entry& entry, std::set<node_id>& output) {
    if (entry.kind == entry_kind::directory) {
        output.insert(entry.node);
    }
    for (const auto& [name, child] : entry.entries) {
        static_cast<void>(name);
        append_working_directories(child, output);
    }
}

auto node_text(const node_id& node) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(node.size() * 2);
    for (const auto octet : node) {
        const auto value = std::to_integer<unsigned char>(octet);
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0f]);
    }
    return output;
}

constexpr auto composition_origin_prefix = std::string_view{"composition.origin."};

auto history_repository(const repository_document& document, const node_id& repository) -> bool {
    return repository == document.repository ||
           document.metadata.contains(std::string{composition_origin_prefix} +
                                      node_text(repository));
}

void append_composition_roots(const working_entry& entry, std::vector<digest>& roots) {
    roots.push_back(entry.object);
    if (entry.nested) {
        roots.push_back(entry.nested->id.value);
    }
    for (const auto& [name, child] : entry.entries) {
        static_cast<void>(name);
        append_composition_roots(child, roots);
    }
}

runtime::task<void>
copy_composition_objects(store& source, store& destination, std::vector<digest> pending) {
    auto visited = std::set<std::string>{};
    const auto registry = store_detail::reference_registry{};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!visited.insert(to_string(id)).second) {
            continue;
        }
        if (id.alg == algorithm::blake3_keyed) {
            co_return std::unexpected(
                failure(errc::unsupported,
                        "composition requires an independent keyed-object policy",
                        to_string(id)));
        }
        auto bytes = co_await source.get(id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        const auto kind = store_detail::detect_object_kind(*bytes);
        const auto references = registry.extract(kind, *bytes);
        pending.insert(pending.end(), references.digests.begin(), references.digests.end());
        auto copied = co_await repository_detail::store_access::put_verified(
            destination, id, std::move(*bytes));
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
    }
    co_return result<void>{};
}

struct split_staging_directory {
    std::filesystem::path path;

    ~split_staging_directory() {
        if (!path.empty()) {
            auto code = std::error_code{};
            std::filesystem::remove_all(path, code);
        }
    }
};

// A process-local directory removed when the merge that needed it ends.
struct scratch_directory {
    std::filesystem::path path;

    scratch_directory() = default;
    scratch_directory(const scratch_directory&) = delete;
    auto operator=(const scratch_directory&) -> scratch_directory& = delete;
    ~scratch_directory() {
        if (!path.empty()) {
            auto code = std::error_code{};
            std::filesystem::remove_all(path, code);
        }
    }

    auto create(std::string_view prefix) -> result<std::filesystem::path> {
        auto code = std::error_code{};
        const auto base = std::filesystem::temp_directory_path(code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), "temporary directory"));
        }
        const auto candidate = base / (std::string{prefix} + node_text(random_id()));
        if (!std::filesystem::create_directory(candidate, code)) {
            return std::unexpected(failure(
                errc::io, code ? code.message() : "scratch directory exists", candidate.string()));
        }
        path = candidate;
        return candidate;
    }
};

auto prepare_split_directory(const std::filesystem::path& destination)
    -> result<std::filesystem::path> {
    auto code = std::error_code{};
    const auto status = std::filesystem::symlink_status(destination, code);
    if (code && code != std::errc::no_such_file_or_directory) {
        return std::unexpected(failure(errc::io, code.message(), destination.string()));
    }
    if (std::filesystem::exists(status)) {
        return std::unexpected(
            failure(errc::already_exists, "split destination exists", destination.string()));
    }
    const auto staging = destination.parent_path() / (".liquid-split-" + node_text(random_id()));
    code.clear();
    if (!std::filesystem::create_directory(staging, code)) {
        return std::unexpected(failure(
            errc::io, code ? code.message() : "split staging directory exists", staging.string()));
    }
    return staging;
}

auto publish_split_directory(const std::filesystem::path& from, const std::filesystem::path& into)
    -> result<void> {
    auto code = std::error_code{};
#if defined(__APPLE__)
    if (::renamex_np(from.c_str(), into.c_str(), RENAME_EXCL) != 0) {
        code = std::error_code{errno, std::generic_category()};
    }
#elif defined(__linux__)
    if (::syscall(
            SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, into.c_str(), RENAME_NOREPLACE) != 0) {
        code = std::error_code{errno, std::generic_category()};
    }
#else
    // Windows directory rename refuses an existing destination.
    std::filesystem::rename(from, into, code);
#endif
    if (code) {
        return std::unexpected(
            failure(code == std::errc::file_exists ? errc::already_exists : errc::io,
                    code.message(),
                    into.string()));
    }
    return {};
}

runtime::task<void>
copy_working_objects(const working_entry& entry, store& source, store& destination) {
    if (entry.kind == entry_kind::file) {
        auto encoded = co_await source.get(entry.object);
        if (!encoded && encoded.error().code == errc::not_found) {
            co_return result<void>{};
        }
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto blob = objects::decode_blob(*encoded);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        auto stored =
            co_await repository_detail::store_access::put_plain(destination, std::move(*encoded));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        for (const auto& chunk : blob->chunks) {
            auto contents = co_await source.get(chunk);
            if (!contents) {
                co_return std::unexpected(contents.error());
            }
            auto copied = co_await destination.put(std::move(*contents));
            if (!copied) {
                co_return std::unexpected(copied.error());
            }
        }
    }
    for (const auto& [name, child] : entry.entries) {
        static_cast<void>(name);
        auto copied = co_await copy_working_objects(child, source, destination);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
    }
    co_return result<void>{};
}

void append_directory_nodes(const working_entry& entry, std::vector<node_id>& output) {
    if (entry.kind == entry_kind::directory) {
        output.push_back(entry.node);
    }
    for (const auto& [name, child] : entry.entries) {
        static_cast<void>(name);
        append_directory_nodes(child, output);
    }
}

auto apply_synced_directory(repository_document& document,
                            const node_id& node,
                            const crdt::directory_document& synced) -> result<void> {
    auto* destination = directory_with_node(document.root, node);
    if (!destination || destination->kind != entry_kind::directory) {
        return std::unexpected(
            failure(errc::corrupt, "sync names an unknown directory", "repository sync"));
    }
    auto entries = synced.entries();
    if (!entries) {
        return std::unexpected(entries.error());
    }
    auto replacement = std::map<std::string, working_entry>{};
    for (const auto& incoming : *entries) {
        auto reference = parse_digest(incoming.value.ref);
        if (!reference) {
            return std::unexpected(reference.error());
        }
        auto current = destination->entries.find(incoming.name);
        auto value = working_entry{};
        if (current != destination->entries.end() && current->second.node == incoming.value.id) {
            value = std::move(current->second);
        } else {
            const auto moved = std::ranges::find_if(destination->entries, [&](const auto& item) {
                return item.second.node == incoming.value.id;
            });
            if (moved != destination->entries.end()) {
                value = std::move(moved->second);
            }
        }
        if (incoming.value.kind == "blob") {
            value.kind = entry_kind::file;
        } else if (incoming.value.kind == "tree") {
            value.kind = entry_kind::directory;
        } else if (incoming.value.kind == "repo") {
            value.kind = entry_kind::nested_repository;
        } else {
            return std::unexpected(
                failure(errc::corrupt, "sync contains an unknown entry kind", incoming.name));
        }
        value.node = incoming.value.id;
        value.mode = incoming.value.mode;
        value.object = *reference;
        replacement.emplace(incoming.name, std::move(value));
    }
    destination->entries = std::move(replacement);
    return {};
}

auto export_directory_messages(const crdt::directory_document& document,
                               const crdt::repo_id& repository,
                               std::string context) -> result<std::vector<std::vector<std::byte>>> {
    auto saved = document.save_sync();
    if (!saved) {
        return std::unexpected(saved.error());
    }
    auto source = crdt::directory_document::load_sync(*saved, "sync-source", context);
    if (!source) {
        saved = document.save_full();
        if (!saved) {
            return std::unexpected(saved.error());
        }
        source = crdt::directory_document::load_sync(*saved, "sync-source", context);
        if (!source) {
            auto problem = source.error();
            problem.context = "prepare directory sync: " + problem.context;
            return std::unexpected(std::move(problem));
        }
    }
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto peer_device = std::string{"repository-sync-receiver-"};
    for (const auto octet : repository) {
        const auto value = std::to_integer<unsigned char>(octet);
        peer_device.push_back(digits[value >> 4]);
        peer_device.push_back(digits[value & 0x0f]);
    }
    auto peer = crdt::directory_document::create_sync_receiver("sync-peer", peer_device);
    if (!peer) {
        return std::unexpected(peer.error());
    }
    auto source_state = crdt::sync_state{};
    auto peer_state = crdt::sync_state{};
    auto output = std::vector<std::vector<std::byte>>{};
    auto opening = peer->generate_sync_message(peer_state);
    if (!opening) {
        return std::unexpected(opening.error());
    }
    if (*opening) {
        auto received = source->receive_sync_message(source_state, **opening);
        if (!received) {
            return std::unexpected(received.error());
        }
    }
    for (auto round = 0; round < 20; ++round) {
        auto progress = false;
        auto outgoing = source->generate_sync_message(source_state);
        if (!outgoing) {
            return std::unexpected(outgoing.error());
        }
        if (*outgoing) {
            output.push_back(**outgoing);
            auto received = peer->receive_sync_message(peer_state, **outgoing);
            if (!received) {
                return std::unexpected(received.error());
            }
            progress = true;
        }
        auto reply = peer->generate_sync_message(peer_state);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        if (*reply) {
            auto received = source->receive_sync_message(source_state, **reply);
            if (!received) {
                return std::unexpected(received.error());
            }
            progress = true;
        }
        if (!progress) {
            return output;
        }
    }
    return std::unexpected(
        failure(errc::corrupt, "directory export did not quiesce", "repository sync"));
}

// True only when an identity chain binds `author` to the key that signed the record. History
// signed by an identity whose chain this machine never received (for example before liquid#448)
// is reported as signed by an unverified key, never as `author` (liquid#429).
auto signed_by_verified_identity(const actor& author,
                                 std::string_view artifact_type,
                                 std::span<const std::byte> body,
                                 std::span<const std::byte> encoded_signature) -> bool {
    auto signature = identity_detail::parse_signature(encoded_signature);
    if (!signature) {
        return false;
    }
    auto checked = check_signature(author, artifact_type, body, *signature);
    return checked && checked->identity_verified;
}

auto change_signed_by_verified_identity(const change& value) -> bool {
    auto body = objects::encode_change_body(value);
    return body && signed_by_verified_identity(value.author, "change", *body, value.sig);
}

// True only when an identity chain binds the drip's signer to its signature, as
// `repository::drips` reports it (liquid#429).
auto drip_signed_by_verified_identity(const persisted_drip& saved, const actor& local) -> bool {
    const auto& signing_author = saved.author.taap_id.empty() ? local : saved.author;
    return signed_by_verified_identity(signing_author, "drip", saved.body, saved.signature);
}

// Says whether an operation record's `by` is verified (liquid#508). An operation record carries
// no signature of its own. It is verified when it was recorded by the local identity, which holds
// the key that signs this journal's drips. An operation attributed to anyone else is verified only
// when a drip that records it (same author, operation and time) checks against that signer's
// identity chain. Otherwise it is shown as `unverified_signer_label`, never as `by`. Each drip is
// decoded and checked at most once per verifier.
class operation_verifier {
public:
    operation_verifier(const repository_document& document, actor local)
        : document_(&document), local_(std::move(local)) {}

    auto verified(const stored_operation& operation) -> bool {
        if (operation.by.taap_id.empty()) {
            return false;
        }
        if (operation.by.taap_id == local_.taap_id) {
            return true;
        }
        index();
        for (auto& candidate : *drips_) {
            if (candidate.by != operation.by.taap_id || candidate.operation != operation.kind ||
                candidate.at != operation.at.time_since_epoch().count()) {
                continue;
            }
            if (!candidate.verified) {
                candidate.verified = drip_signed_by_verified_identity(*candidate.source, local_);
            }
            if (*candidate.verified) {
                return true;
            }
        }
        return false;
    }

private:
    struct recorded_drip {
        std::string by;
        std::string operation;
        std::int64_t at{};
        const persisted_drip* source{};
        std::optional<bool> verified;
    };

    auto index() -> void {
        if (drips_) {
            return;
        }
        drips_.emplace();
        drips_->reserve(document_->drips.size());
        for (const auto& saved : document_->drips) {
            auto decoded = decode_repository_drip(saved.body);
            if (!decoded) {
                continue;
            }
            const auto& signer = saved.author.taap_id.empty() ? local_ : saved.author;
            drips_->push_back({.by = decoded->agent ? decoded->agent->taap_id : signer.taap_id,
                               .operation = decoded->operation,
                               .at = decoded->at,
                               .source = &saved});
        }
    }

    const repository_document* document_;
    actor local_;
    std::optional<std::vector<recorded_drip>> drips_;
};

// The author of an imported drip or change is accepted only through `verify`, which requires the
// author's identity chain (liquid#429): a record signed only by the key it carries is refused.
auto import_synced_drip(repository_document& candidate,
                        std::vector<persisted_drip>& imported_drips,
                        const sync_item& item) -> result<void> {
    auto body = cbor::decode(item.body);
    const auto* author_value = body ? find(*body, "author") : nullptr;
    const auto* author = author_value ? std::get_if<std::string>(&author_value->data) : nullptr;
    if (!body || !author || *author != item.author.taap_id) {
        return std::unexpected(failure(errc::corrupt, "drip author does not match its body"));
    }
    auto signature = identity_detail::parse_signature(item.signature);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    auto valid = verify(item.author, "drip", item.body, *signature);
    if (!valid || !*valid) {
        return std::unexpected(valid ? failure(errc::unauthorized, "unsigned drip")
                                     : valid.error());
    }
    const auto duplicate = std::ranges::any_of(candidate.drips, [&](const auto& value) {
        return value.body == item.body && value.signature == item.signature;
    });
    if (!duplicate) {
        imported_drips.push_back({item.author, item.body, item.signature});
    }
    return {};
}

// The repository keeps every author's identity chain in its own metadata, so it can send the
// chain with that author's records after the process's identity store is gone (a new container
// has a new home). A chain is self-verifying from its identifier, so the receiver re-verifies it.
constexpr std::string_view identity_chain_metadata_prefix = "liquid.identity-chain.";

auto identity_chain_metadata_key(std::string_view taap_id) -> std::string {
    return std::string{identity_chain_metadata_prefix} + std::string{taap_id};
}

// Keeps the longer of the stored and offered encoded chains for `taap_id` in `metadata`.
auto remember_identity_chain(std::map<std::string, std::vector<std::byte>>& metadata,
                             std::string_view taap_id,
                             std::span<const identity_document> chain) -> void {
    auto encoded = encode_identity_chain(chain);
    if (!encoded) {
        return;
    }
    auto& slot = metadata[identity_chain_metadata_key(taap_id)];
    if (!slot.empty()) {
        auto stored = decode_identity_chain(slot);
        if (stored && stored->size() >= chain.size()) {
            return;
        }
    }
    slot = std::move(*encoded);
}

// An author's identity chain arrives ahead of the drips and changes it attributes. It is
// verified and stored, so the signature checks that follow can bind the author's name to a key
// (liquid#383). A rotation not signed by the key it replaces is refused here; only an out-of-band
// `liquid identity trust --accept-compromise-rotation` accepts one.
auto import_synced_identity(repository_document& candidate, const sync_item& item) -> result<void> {
    auto chain = decode_identity_chain(item.body);
    if (!chain) {
        return std::unexpected(chain.error());
    }
    if (chain->front().subject.taap_id != item.author.taap_id) {
        return std::unexpected(
            failure(errc::corrupt, "identity chain does not match its frame author"));
    }
    auto trusted = trust_identity_chain(*chain);
    if (!trusted) {
        return std::unexpected(trusted.error());
    }
    auto stored = load_identity_chain(item.author.taap_id);
    remember_identity_chain(candidate.metadata, item.author.taap_id, stored ? *stored : *chain);
    return {};
}

runtime::task<void>
import_synced_change(store& objects, algorithm hash_algorithm, const sync_item& item) {
    auto value = objects::decode_change(item.body, hash_algorithm);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto body = objects::encode_change_body(*value);
    auto signature = identity_detail::parse_signature(value->sig);
    if (!body || !signature) {
        co_return std::unexpected(body ? signature.error() : body.error());
    }
    if (value->author.taap_id != item.author.taap_id) {
        co_return std::unexpected(failure(errc::corrupt, "change author does not match its frame"));
    }
    auto valid = verify(item.author, "change", *body, *signature);
    if (!valid || !*valid) {
        co_return std::unexpected(valid ? failure(errc::unauthorized, "unsigned change")
                                        : valid.error());
    }
    auto stored = co_await repository_detail::store_access::put_plain(objects, item.body);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!same_digest(*stored, item.reference.value)) {
        co_return std::unexpected(failure(errc::corrupt, "change digest does not match its frame"));
    }
    co_return result<void>{};
}

auto import_synced_directory(const node_id& repository,
                             std::map<node_id, crdt::directory_document>& candidate_documents,
                             std::map<node_id, std::unique_ptr<crdt::sync_state>>& directory_states,
                             std::string_view device_id,
                             const sync_item& item) -> result<void> {
    if (item.repository != repository) {
        return std::unexpected(failure(errc::conflict, "directory repository identity differs"));
    }
    auto found = candidate_documents.find(item.directory);
    if (found == candidate_documents.end()) {
        auto created = crdt::directory_document::create_sync_receiver("local", device_id);
        if (!created) {
            return std::unexpected(created.error());
        }
        found = candidate_documents.emplace(item.directory, std::move(*created)).first;
    }
    auto state = directory_states.find(item.directory);
    if (state == directory_states.end()) {
        state =
            directory_states.emplace(item.directory, std::make_unique<crdt::sync_state>()).first;
    }
    auto received = found->second.receive_sync_message(*state->second, item.body);
    if (!received) {
        return std::unexpected(received.error());
    }
    auto acknowledgement = found->second.generate_sync_message(*state->second);
    if (!acknowledgement) {
        return std::unexpected(acknowledgement.error());
    }
    return {};
}

struct historical_merge_file {
    std::string path;
    digest reference;
};

struct working_merge_file {
    std::string path;
    std::string name;
    node_id parent{};
    working_entry* entry{};
};

void collect_working_merge_files(working_entry& directory,
                                 std::string prefix,
                                 std::map<node_id, working_merge_file>& output) {
    for (auto& [name, entry] : directory.entries) {
        const auto path = prefix.empty() ? name : prefix + "/" + name;
        if (entry.kind == entry_kind::file) {
            output[entry.node] = {path, name, directory.node, &entry};
        } else if (entry.kind == entry_kind::directory) {
            collect_working_merge_files(entry, path, output);
        }
    }
}

void collect_const_working_merge_files(const working_entry& directory,
                                       std::string prefix,
                                       std::map<node_id, historical_merge_file>& output) {
    for (const auto& [name, entry] : directory.entries) {
        const auto path = prefix.empty() ? name : prefix + "/" + name;
        if (entry.kind == entry_kind::file) {
            output[entry.node] = {path, entry.object};
        } else if (entry.kind == entry_kind::directory) {
            collect_const_working_merge_files(entry, path, output);
        }
    }
}

runtime::task<std::map<node_id, historical_merge_file>> files_at_change(store& objects,
                                                                        change_id revision) {
    auto encoded = co_await objects.get(revision.value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto change = objects::decode_change(*encoded);
    if (!change) {
        co_return std::unexpected(change.error());
    }
    auto pending = std::vector<std::pair<digest, std::string>>{{change->root, {}}};
    auto output = std::map<node_id, historical_merge_file>{};
    while (!pending.empty()) {
        auto [tree_id, prefix] = std::move(pending.back());
        pending.pop_back();
        auto tree_bytes = co_await objects.get(tree_id);
        if (!tree_bytes) {
            co_return std::unexpected(tree_bytes.error());
        }
        auto tree = objects::decode_tree(*tree_bytes);
        if (!tree) {
            co_return std::unexpected(tree.error());
        }
        for (const auto& entry : tree->entries) {
            const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
            if (entry.kind == node_kind::blob) {
                output[entry.node] = {path, entry.ref};
            } else if (entry.kind == node_kind::tree) {
                pending.emplace_back(entry.ref, path);
            }
        }
    }
    co_return output;
}

runtime::task<std::optional<change_id>>
common_change_ancestor(store& objects, const node_id& repository, change_id left, change_id right) {
    auto left_distances = std::map<std::string, std::size_t>{};
    auto pending = std::vector<std::pair<change_id, std::size_t>>{{left, 0}};
    for (auto cursor = std::size_t{}; cursor < pending.size(); ++cursor) {
        const auto [current, distance] = pending[cursor];
        const auto key = to_string(current.value);
        if (!left_distances.emplace(key, distance).second) {
            continue;
        }
        auto bytes = co_await objects.get(current.value);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_change(*bytes);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        for (const auto& parent : value->parents) {
            if (parent.repo_id == repository) {
                pending.emplace_back(parent.id, distance + 1);
            }
        }
    }
    pending = {{right, 0}};
    auto visited = std::set<std::string>{};
    auto best = std::optional<change_id>{};
    auto best_score =
        std::pair{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max()};
    for (auto cursor = std::size_t{}; cursor < pending.size(); ++cursor) {
        const auto [current, distance] = pending[cursor];
        const auto key = to_string(current.value);
        if (!visited.insert(key).second) {
            continue;
        }
        if (const auto found = left_distances.find(key); found != left_distances.end()) {
            const auto score =
                std::pair{std::max(found->second, distance), found->second + distance};
            if (score < best_score) {
                best = current;
                best_score = score;
            }
        }
        auto bytes = co_await objects.get(current.value);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_change(*bytes);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        for (const auto& parent : value->parents) {
            if (parent.repo_id == repository) {
                pending.emplace_back(parent.id, distance + 1);
            }
        }
    }
    co_return best;
}

runtime::task<digest> store_merge_blob(store& objects, std::vector<std::byte> contents) {
    auto content = co_await objects.put(std::move(contents));
    if (!content) {
        co_return std::unexpected(content.error());
    }
    auto record = repository_detail::store_access::blob_for(objects, *content);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    record->content = {};
    auto encoded = objects::encode(*record);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
}

runtime::task<std::vector<std::byte>>
load_merge_blob(store& objects, digest reference, std::string path) {
    auto encoded = co_await objects.get(reference);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = objects::decode_blob(*encoded);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    if (record->content.alg == algorithm::blake3 || record->content.alg == algorithm::sha256 ||
        record->content.alg == algorithm::blake3_keyed) {
        co_return co_await objects.get(record->content);
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(record->size));
    for (const auto& chunk : record->chunks) {
        auto content = co_await objects.get(chunk);
        if (!content) {
            co_return std::unexpected(content.error());
        }
        output.insert(output.end(), content->begin(), content->end());
    }
    if (output.size() != record->size) {
        co_return std::unexpected(
            failure(errc::corrupt, "blob size does not match content", std::move(path)));
    }
    co_return output;
}

runtime::task<digest> store_merge_conflict(store& objects,
                                           repository_document& candidate,
                                           working_merge_file& selected,
                                           conflict value) {
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto stored = co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    std::erase_if(candidate.conflict_values, [&](const conflict_reference& existing) {
        return existing.path == selected.path;
    });
    candidate.conflict_values.push_back({.path = selected.path,
                                         .id = *stored,
                                         .selected = selected.entry->object,
                                         .selected_kind = node_kind::blob});
    co_return *stored;
}

auto structural_fallback(const error& value) -> bool {
    return value.code == errc::unsupported || value.code == errc::not_found;
}

runtime::task<void> store_text_merge_conflict(store& objects,
                                              repository_document& candidate,
                                              working_merge_file& selected,
                                              conflict value,
                                              std::string explanation,
                                              std::string node) {
    value.kind = conflict_kind::text;
    value.explanation = std::move(explanation);
    value.reasons = {value.explanation};
    value.node = std::move(node);
    auto stored = co_await store_merge_conflict(objects, candidate, selected, std::move(value));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    co_return result<void>{};
}

runtime::task<void>
update_merged_entry(working_merge_file& selected,
                    digest reference,
                    std::map<node_id, crdt::directory_document>& candidate_documents) {
    const auto document = candidate_documents.find(selected.parent);
    if (document == candidate_documents.end()) {
        co_return std::unexpected(
            failure(errc::corrupt, "merged file parent document is absent", selected.path));
    }
    const auto now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto recorded = document->second.put_entry(
        selected.name,
        {"blob", selected.entry->node, selected.entry->mode, to_string(reference)},
        crdt::commit_options{.message = "structural merge",
                             .time = now.time_since_epoch().count()});
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    selected.entry->object = reference;
    co_return result<void>{};
}

runtime::task<void>
apply_structural_merges(store& objects,
                        const repository_document& before,
                        repository_document& candidate,
                        std::map<node_id, crdt::directory_document>& candidate_documents,
                        const std::map<node_id, digest>& remote_writes,
                        std::optional<change_id> remote_head,
                        frontend_graph* graph,
                        std::map<std::string, std::string>& operation_metadata) {
    if (!before.head || !remote_head || remote_writes.empty()) {
        co_return result<void>{};
    }
    auto ancestor =
        co_await common_change_ancestor(objects, before.repository, *before.head, *remote_head);
    if (!ancestor) {
        co_return std::unexpected(ancestor.error());
    }
    auto local_files = std::map<node_id, historical_merge_file>{};
    collect_const_working_merge_files(before.root, {}, local_files);
    auto selected_files = std::map<node_id, working_merge_file>{};
    collect_working_merge_files(candidate.root, {}, selected_files);
    if (!*ancestor) {
        for (const auto& [node, remote_reference] : remote_writes) {
            const auto local = local_files.find(node);
            auto selected = selected_files.find(node);
            if (local == local_files.end() || selected == selected_files.end() ||
                same_digest(local->second.reference, remote_reference)) {
                continue;
            }
            auto value = repository_detail::entry_conflict(
                selected->second.path, local->second.reference, remote_reference, node, node);
            if (value) {
                auto stored = co_await store_merge_conflict(
                    objects, candidate, selected->second, std::move(*value));
                if (!stored) {
                    co_return std::unexpected(stored.error());
                }
            }
        }
        co_return result<void>{};
    }
    auto base_files = co_await files_at_change(objects, **ancestor);
    if (!base_files) {
        co_return std::unexpected(base_files.error());
    }
    auto merge_index = std::size_t{};
    for (const auto& [node, remote_reference] : remote_writes) {
        const auto base = base_files->find(node);
        const auto local = local_files.find(node);
        auto selected = selected_files.find(node);
        if (base == base_files->end() || local == local_files.end() ||
            selected == selected_files.end() ||
            same_digest(base->second.reference, local->second.reference) ||
            same_digest(base->second.reference, remote_reference) ||
            same_digest(local->second.reference, remote_reference)) {
            continue;
        }
        auto base_bytes =
            co_await load_merge_blob(objects, base->second.reference, selected->second.path);
        auto left_bytes =
            co_await load_merge_blob(objects, local->second.reference, selected->second.path);
        auto right_bytes =
            co_await load_merge_blob(objects, remote_reference, selected->second.path);
        if (!base_bytes || !left_bytes || !right_bytes) {
            co_return std::unexpected(!base_bytes   ? base_bytes.error()
                                      : !left_bytes ? left_bytes.error()
                                                    : right_bytes.error());
        }
        auto value = conflict{
            .base = base->second.reference,
            .left = local->second.reference,
            .right = remote_reference,
            .region = {.path = selected->second.path, .nodes = conflict_node_range{node, node}}};
        const auto record_metadata = [&](std::string algorithm, std::string grammar) {
            const auto prefix =
                merge_index == 0 ? std::string{} : "merge." + std::to_string(merge_index) + ".";
            operation_metadata[prefix + "algorithm"] = std::move(algorithm);
            operation_metadata[prefix + "grammar"] = std::move(grammar);
            operation_metadata[prefix + "base"] = to_string(base->second.reference);
            operation_metadata[prefix + "left"] = to_string(local->second.reference);
            operation_metadata[prefix + "right"] = to_string(remote_reference);
            operation_metadata[prefix + "path"] = selected->second.path;
            ++merge_index;
        };
        auto language = graph ? graph->language_for_path(selected->second.path)
                              : result<frontend_language>{std::unexpected(failure(
                                    errc::not_found, "structural frontend is unavailable"))};
        if (!language) {
            auto stored = co_await store_text_merge_conflict(
                objects,
                candidate,
                selected->second,
                std::move(value),
                "text file has no grammar; both sides changed node " + node_text(node),
                node_text(node));
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            record_metadata("entry-three-way-v1", "text");
            continue;
        }
        auto base_digest = hash(algorithm::blake3, *base_bytes);
        auto left_digest = hash(algorithm::blake3, *left_bytes);
        auto right_digest = hash(algorithm::blake3, *right_bytes);
        if (!base_digest || !left_digest || !right_digest) {
            co_return std::unexpected(!base_digest   ? base_digest.error()
                                      : !left_digest ? left_digest.error()
                                                     : right_digest.error());
        }
        auto base_tree = co_await graph->parse(
            {.language = language->language, .blob = *base_digest, .source = *base_bytes});
        auto left_tree = co_await graph->parse(
            {.language = language->language, .blob = *left_digest, .source = *left_bytes});
        auto right_tree = co_await graph->parse(
            {.language = language->language, .blob = *right_digest, .source = *right_bytes});
        if (!base_tree || !left_tree || !right_tree) {
            const auto& parse_error = !base_tree   ? base_tree.error()
                                      : !left_tree ? left_tree.error()
                                                   : right_tree.error();
            if (structural_fallback(parse_error)) {
                auto stored = co_await store_text_merge_conflict(
                    objects,
                    candidate,
                    selected->second,
                    std::move(value),
                    "text structural merge fallback for " + language->language + ": " +
                        parse_error.message,
                    node_text(node));
                if (!stored) {
                    co_return std::unexpected(stored.error());
                }
                record_metadata("entry-three-way-v1", "text:" + language->language);
                continue;
            }
            co_return std::unexpected(!base_tree   ? base_tree.error()
                                      : !left_tree ? left_tree.error()
                                                   : right_tree.error());
        }
        auto symbol_request = [&](const std::vector<std::byte>& source) {
            return frontend_build_request{.language = language->language,
                                          .source_path = selected->second.path,
                                          .source = source};
        };
        auto base_symbols = co_await graph->symbols(symbol_request(*base_bytes));
        auto left_symbols = co_await graph->symbols(symbol_request(*left_bytes));
        auto right_symbols = co_await graph->symbols(symbol_request(*right_bytes));
        if (!base_symbols || !left_symbols || !right_symbols) {
            const auto& symbol_error = !base_symbols   ? base_symbols.error()
                                       : !left_symbols ? left_symbols.error()
                                                       : right_symbols.error();
            if (structural_fallback(symbol_error)) {
                auto stored = co_await store_text_merge_conflict(
                    objects,
                    candidate,
                    selected->second,
                    std::move(value),
                    "text structural merge fallback for " + language->language + ": " +
                        symbol_error.message,
                    node_text(node));
                if (!stored) {
                    co_return std::unexpected(stored.error());
                }
                record_metadata("entry-three-way-v1", "text:" + language->language);
                continue;
            }
            co_return std::unexpected(!base_symbols   ? base_symbols.error()
                                      : !left_symbols ? left_symbols.error()
                                                      : right_symbols.error());
        }
        auto offloaded = co_await runtime::offload([base_tree = std::move(*base_tree),
                                                    left_tree = std::move(*left_tree),
                                                    right_tree = std::move(*right_tree),
                                                    base_symbols = std::move(*base_symbols),
                                                    left_symbols = std::move(*left_symbols),
                                                    right_symbols = std::move(*right_symbols),
                                                    base_bytes = *base_bytes,
                                                    left_bytes = *left_bytes,
                                                    right_bytes = *right_bytes]() mutable {
            return repository_detail::merge_structural(base_tree,
                                                       left_tree,
                                                       right_tree,
                                                       base_bytes,
                                                       left_bytes,
                                                       right_bytes,
                                                       &base_symbols,
                                                       &left_symbols,
                                                       &right_symbols);
        });
        if (!offloaded) {
            co_return std::unexpected(offloaded.error());
        }
        auto merged = std::move(*offloaded);
        if (!merged) {
            co_return std::unexpected(merged.error());
        }
        if (merged->outcome != repository_detail::structural_merge_outcome::conflict) {
            auto proposal_digest = hash(algorithm::blake3, merged->proposal);
            if (!proposal_digest) {
                co_return std::unexpected(proposal_digest.error());
            }
            auto proposal_tree = co_await graph->parse({.language = language->language,
                                                        .blob = *proposal_digest,
                                                        .source = merged->proposal});
            if (!proposal_tree) {
                co_return std::unexpected(proposal_tree.error());
            }
            if (repository_detail::has_recovery_node(proposal_tree->root)) {
                merged->outcome = repository_detail::structural_merge_outcome::suspicious;
                merged->reasons.push_back("merged syntax contains ERROR or MISSING nodes");
            }
        }
        if (merged->outcome == repository_detail::structural_merge_outcome::clean) {
            auto stored = co_await store_merge_blob(objects, std::move(merged->proposal));
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            auto updated =
                co_await update_merged_entry(selected->second, *stored, candidate_documents);
            if (!updated) {
                co_return std::unexpected(updated.error());
            }
        } else {
            value.kind = merged->outcome == repository_detail::structural_merge_outcome::suspicious
                             ? conflict_kind::suspicious_merge
                             : conflict_kind::structural;
            value.reasons = merged->reasons;
            value.explanation =
                value.reasons.empty() ? "structural edits overlap" : value.reasons.front();
            value.node = merged->node.value_or(node_text(node));
            if (value.kind == conflict_kind::suspicious_merge) {
                auto proposal = co_await store_merge_blob(objects, std::move(merged->proposal));
                if (!proposal) {
                    co_return std::unexpected(proposal.error());
                }
                value.proposal = *proposal;
            }
            auto stored = co_await store_merge_conflict(
                objects, candidate, selected->second, std::move(value));
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
        }
        record_metadata(merged->algorithm, merged->grammar);
    }
    co_return result<void>{};
}

struct sync_import_state {
    repository_document& candidate;
    store& objects;
    algorithm hash_algorithm;
    std::map<node_id, crdt::directory_document>& candidate_documents;
    std::map<node_id, std::unique_ptr<crdt::sync_state>>& directory_states;
    std::vector<persisted_drip>& imported_drips;
    std::vector<std::pair<std::string, change_id>>& imported_refs;
    std::string device_id;
};

runtime::task<void> import_sync_item(sync_import_state& state, const sync_item& item) {
    switch (item.kind) {
    case sync_item_kind::drip:
        co_return import_synced_drip(state.candidate, state.imported_drips, item);
    case sync_item_kind::change:
        co_return co_await import_synced_change(state.objects, state.hash_algorithm, item);
    case sync_item_kind::directory:
        co_return import_synced_directory(state.candidate.repository,
                                          state.candidate_documents,
                                          state.directory_states,
                                          state.device_id,
                                          item);
    case sync_item_kind::reference:
        if (!repository_detail::valid_ref_name(item.name)) {
            co_return std::unexpected(failure(
                errc::invalid_argument, "invalid reference name (git check-ref-format)", item.name));
        }
        state.imported_refs.emplace_back(item.name, item.reference);
        co_return result<void>{};
    case sync_item_kind::presence:
        co_return std::unexpected(
            failure(errc::corrupt, "presence reached the persistent sync importer"));
    case sync_item_kind::identity:
        co_return import_synced_identity(state.candidate, item);
    }
    co_return std::unexpected(failure(errc::corrupt, "unknown sync item kind"));
}

auto apply_synced_directories(
    repository_document& candidate,
    const std::map<node_id, crdt::directory_document>& candidate_documents) -> result<void> {
    if (auto root = candidate_documents.find(candidate.root.node);
        root != candidate_documents.end()) {
        auto applied = apply_synced_directory(candidate, root->first, root->second);
        if (!applied) {
            return std::unexpected(applied.error());
        }
    }
    auto pending = std::set<node_id>{};
    for (const auto& [node, document] : candidate_documents) {
        static_cast<void>(document);
        if (node != candidate.root.node) {
            pending.insert(node);
        }
    }
    for (;;) {
        auto progress = false;
        for (auto current = pending.begin(); current != pending.end();) {
            if (!directory_with_node(candidate.root, *current)) {
                ++current;
                continue;
            }
            auto applied = apply_synced_directory(
                candidate, *current, candidate_documents.find(*current)->second);
            if (!applied) {
                return std::unexpected(applied.error());
            }
            current = pending.erase(current);
            progress = true;
        }
        if (pending.empty() || !progress) {
            break;
        }
    }
    if (!pending.empty()) {
        auto context = std::string{"root="} + node_text(candidate.root.node) + "; pending=";
        for (const auto& node : pending) {
            context += node_text(node) + ",";
        }
        context += "; root entries=";
        for (const auto& [name, entry] : candidate.root.entries) {
            context += name + ":" + std::to_string(static_cast<unsigned>(entry.kind)) + ":" +
                       node_text(entry.node) + ",";
        }
        return std::unexpected(
            failure(errc::corrupt, "sync contains unreachable directory documents", context));
    }
    return {};
}

runtime::task<void>
apply_synced_refs(store& objects,
                  repository_document& candidate,
                  const std::vector<std::pair<std::string, change_id>>& imported_refs) {
    for (const auto& [name, id] : imported_refs) {
        auto present = co_await objects.contains(id.value);
        if (!present || !*present) {
            co_return std::unexpected(
                present ? failure(errc::corrupt, "reference change is absent", name)
                        : present.error());
        }
        candidate.refs[name] = id;
        if (name == "main") {
            candidate.head = id;
            auto encoded = co_await objects.get(id.value);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto change = objects::decode_change(*encoded);
            if (!change) {
                co_return std::unexpected(change.error());
            }
            candidate.conflict_values = change->conflicts;
            for (auto& conflict : candidate.conflict_values) {
                if (conflict.id.alg == algorithm{}) {
                    continue;
                }
                auto parts = split_path(conflict.path);
                if (!parts) {
                    co_return std::unexpected(parts.error());
                }
                const auto* selected = lookup(candidate.root, *parts);
                if (!selected) {
                    co_return std::unexpected(
                        failure(errc::corrupt, "conflict path is absent", conflict.path));
                }
                conflict.selected = selected->object;
                conflict.selected_kind = node_kind::blob;
                if (selected->kind == entry_kind::directory) {
                    conflict.selected_kind = node_kind::tree;
                } else if (selected->kind == entry_kind::nested_repository) {
                    conflict.selected_kind = node_kind::repo;
                }
            }
        }
    }
    co_return result<void>{};
}

auto same_path_or_descendant(std::string_view path, std::string_view root) -> bool {
    return path == root ||
           (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
}

auto locks_overlap(std::string_view left, std::string_view right) -> bool {
    return same_path_or_descendant(left, right) || same_path_or_descendant(right, left);
}

auto encode_lock_record(const exclusive_lock& value) -> result<std::vector<std::byte>> {
    return cbor::encode(map(
        {{"v", cbor::value{std::uint64_t{1}}}, {"kind", "lock"}, {"value", lock_value(value)}}));
}

auto validate_lock_config(const lock_config& config) -> result<void>;

runtime::task<void> reconcile_lock_drips(repository_document& candidate,
                                         std::span<const persisted_drip> drips,
                                         store& objects) {
    for (const auto& saved : drips) {
        auto decoded = decode_repository_drip(saved.body);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        if (decoded->lock_rules) {
            auto valid = validate_lock_config(*decoded->lock_rules);
            if (!valid) {
                co_return std::unexpected(valid.error());
            }
            candidate.lock_rules = std::move(*decoded->lock_rules);
        }
        if (!decoded->lock) {
            continue;
        }
        if (decoded->lock->holder.taap_id != saved.author.taap_id) {
            co_return std::unexpected(failure(errc::unauthorized,
                                              "signed lock holder does not match its author",
                                              decoded->lock->path));
        }
        auto existing =
            decoded->operation == "lock"
                ? std::ranges::find_if(candidate.locks,
                                       [&](const auto& value) {
                                           return locks_overlap(value.path, decoded->lock->path);
                                       })
                : std::ranges::find(candidate.locks, decoded->lock->path, &exclusive_lock::path);
        if (decoded->operation == "lock") {
            if (existing == candidate.locks.end()) {
                candidate.locks.push_back(std::move(*decoded->lock));
                continue;
            }
            if (existing->holder.taap_id == decoded->lock->holder.taap_id) {
                continue;
            }
        } else if (decoded->operation == "unlock") {
            if (existing == candidate.locks.end()) {
                continue;
            }
            if (existing->holder.taap_id == decoded->lock->holder.taap_id) {
                candidate.locks.erase(existing);
                continue;
            }
        } else {
            continue;
        }
        auto local_record = encode_lock_record(*existing);
        auto remote_record = encode_lock_record(*decoded->lock);
        if (!local_record || !remote_record) {
            co_return std::unexpected(local_record ? remote_record.error() : local_record.error());
        }
        auto local_id =
            co_await repository_detail::store_access::put_plain(objects, std::move(*local_record));
        auto remote_id =
            co_await repository_detail::store_access::put_plain(objects, std::move(*remote_record));
        if (!local_id || !remote_id) {
            co_return std::unexpected(local_id ? remote_id.error() : local_id.error());
        }
        const auto duplicate =
            std::ranges::any_of(candidate.conflict_values, [&](const auto& value) {
                return value.path == decoded->lock->path;
            });
        if (!duplicate) {
            auto value =
                conflict{.base = *local_id,
                         .left = *local_id,
                         .right = *remote_id,
                         .region = {.path = decoded->lock->path, .bytes = conflict_byte_range{}},
                         .kind = conflict_kind::crdt_suspicious,
                         .explanation = "concurrent lock claims disagree"};
            auto encoded = objects::encode(value);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto id =
                co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
            if (!id) {
                co_return std::unexpected(id.error());
            }
            candidate.conflict_values.push_back({.path = decoded->lock->path,
                                                 .id = *id,
                                                 .selected = *local_id,
                                                 .selected_kind = node_kind::blob});
        }
    }
    co_return result<void>{};
}

auto glob_matches(std::string_view pattern, std::string_view path) -> bool {
    auto previous = std::vector<bool>(path.size() + 1);
    previous[0] = true;
    for (std::size_t pattern_index = 0; pattern_index < pattern.size();) {
        auto current = std::vector<bool>(path.size() + 1);
        if (pattern[pattern_index] == '*') {
            const auto recursive =
                pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
            pattern_index += recursive ? 2 : 1;
            current[0] = previous[0];
            for (std::size_t path_index = 1; path_index <= path.size(); ++path_index) {
                const auto consumable = recursive || path[path_index - 1] != '/';
                current[path_index] =
                    previous[path_index] || (consumable && current[path_index - 1]);
            }
        } else {
            for (std::size_t path_index = 1; path_index <= path.size(); ++path_index) {
                const auto character_matches =
                    pattern[pattern_index] == '?' || pattern[pattern_index] == path[path_index - 1];
                const auto allowed = pattern[pattern_index] != '?' || path[path_index - 1] != '/';
                current[path_index] = previous[path_index - 1] && character_matches && allowed;
            }
            ++pattern_index;
        }
        previous = std::move(current);
    }
    return previous[path.size()];
}

auto validate_lock_config(const lock_config& config) -> result<void> {
    auto names = std::set<std::string>{};
    for (const auto& rule : config.rules) {
        if (rule.name.empty() || !names.insert(rule.name).second || rule.paths.empty()) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid or duplicate lock rule", rule.name));
        }
        for (const auto& path : rule.paths) {
            if (path.empty() || path.front() == '/' || path.back() == '/' ||
                path.find("../") != std::string::npos || path == "..") {
                return std::unexpected(
                    failure(errc::invalid_argument, "invalid lock path pattern", path));
            }
        }
    }
    return {};
}

auto validate_encryption_rules(const std::vector<encryption_path_rule>& rules) -> result<void> {
    for (const auto& rule : rules) {
        if (rule.path.empty() || rule.path.front() == '/' || rule.key_id.empty()) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid encryption path rule", rule.path));
        }
        if (rule.key_id.find_first_of(":]") != std::string::npos) {
            return std::unexpected(failure(errc::invalid_argument,
                                           "encryption key id cannot contain ':' or ']'",
                                           rule.key_id));
        }
        if (rule.mode == encryption_mode::leaf) {
            try {
                static_cast<void>(std::regex{rule.encrypted_regex});
            } catch (const std::regex_error& caught) {
                return std::unexpected(
                    failure(errc::invalid_argument, caught.what(), rule.encrypted_regex));
            }
        }
    }
    return {};
}

auto validate_retention_rules(const retention_config& config) -> result<void> {
    auto names = std::set<std::string>{};
    auto result_rules = std::uint32_t{};
    auto cap_rules = std::uint32_t{};
    for (const auto& configured : config.rules) {
        auto valid = std::visit(
            [&](const auto& rule) -> result<void> {
                using rule_type = std::decay_t<decltype(rule)>;
                if (rule.name.empty() || rule.name.size() > 128 || rule.name.contains('\n') ||
                    !names.insert(rule.name).second) {
                    return std::unexpected(failure(errc::invalid_argument,
                                                   "invalid or duplicate retention rule name",
                                                   rule.name));
                }
                if constexpr (std::is_same_v<rule_type, ref_retention_rule>) {
                    if (rule.glob.empty() ||
                        (rule.duration && *rule.duration <= std::chrono::seconds::zero())) {
                        return std::unexpected(failure(
                            errc::invalid_argument, "invalid ref retention rule", rule.name));
                    }
                } else if constexpr (std::is_same_v<rule_type, result_retention_rule>) {
                    ++result_rules;
                    if (rule.base <= std::chrono::seconds::zero()) {
                        return std::unexpected(failure(
                            errc::invalid_argument, "invalid result retention rule", rule.name));
                    }
                } else {
                    ++cap_rules;
                    if (rule.bytes == 0) {
                        return std::unexpected(failure(
                            errc::invalid_argument, "invalid size-cap retention rule", rule.name));
                    }
                }
                return {};
            },
            configured);
        if (!valid) {
            return valid;
        }
    }
    if (result_rules > 1 || cap_rules > 1) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "retention accepts at most one result and one size-cap rule",
                                       "config.retention"));
    }
    return {};
}

auto validate_agents_config(const agents_config& config) -> result<void> {
    auto actors = std::set<std::string>{};
    for (const auto& actor : config.actors) {
        if (actor.empty() || !actors.insert(actor).second) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid or duplicate agent actor", actor));
        }
    }
    return {};
}

auto validate_data_config(const data_config& config) -> result<void> {
    auto names = std::set<std::string>{};
    auto environments = std::set<std::string>{};
    for (const auto& database : config.databases) {
        const auto valid_name = !database.name.empty() && database.name.size() <= 63 &&
                                std::ranges::all_of(database.name, [](const auto character) {
                                    return std::isalnum(static_cast<unsigned char>(character)) ||
                                           character == '_' || character == '-';
                                });
        const auto valid_environment =
            !database.url_env.empty() &&
            std::ranges::all_of(database.url_env,
                                [](const auto character) {
                                    return std::isalnum(static_cast<unsigned char>(character)) ||
                                           character == '_';
                                }) &&
            !std::isdigit(static_cast<unsigned char>(database.url_env.front()));
        const auto valid_url = database.base_url.starts_with("postgresql://") ||
                               database.base_url.starts_with("postgres://");
        if (!valid_name || !names.insert(database.name).second) {
            return std::unexpected(failure(
                errc::invalid_argument, "invalid or duplicate data database name", database.name));
        }
        if (!valid_environment || !environments.insert(database.url_env).second) {
            return std::unexpected(failure(errc::invalid_argument,
                                           "invalid or duplicate data URL environment",
                                           database.url_env));
        }
        if (!valid_url) {
            return std::unexpected(failure(
                errc::invalid_argument, "data database requires a PostgreSQL URL", database.name));
        }
    }
    return {};
}

auto validate_hot_update_config(const hot_update_config& config) -> result<void> {
    if (std::ranges::any_of(config.restart_hook, &std::string::empty)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "hot-update restart hook has an empty argument",
                                       "config.hot_update.restart_hook"));
    }
    auto names = std::set<std::string>{};
    for (const auto& target : config.targets) {
        if (target.name.empty() || target.endpoint.empty() || !names.insert(target.name).second) {
            return std::unexpected(failure(
                errc::invalid_argument, "invalid or duplicate hot-update target", target.name));
        }
    }
    if (!config.targets.empty() && config.restart_hook.empty()) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "configured hot-update targets require a restart hook",
                                       "config.hot_update.restart_hook"));
    }
    return {};
}

auto encryption_rule_for(const repository_document& document, std::string_view path)
    -> const encryption_path_rule* {
    for (const auto& rule : document.encryption_rules) {
        if (glob_matches(rule.path, path)) {
            return &rule;
        }
    }
    return nullptr;
}

auto leaf_format(std::string_view path) -> std::string_view {
    if (path.ends_with(".json")) {
        return "json";
    }
    if (path.ends_with(".env")) {
        return "dotenv";
    }
    return {};
}

auto string_bytes(std::string_view input) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(input.data()),
            reinterpret_cast<const std::byte*>(input.data() + input.size())};
}

auto identifier_key(const std::filesystem::path& identifier) -> result<std::string> {
    const auto text = identifier.generic_string();
    if (text.empty()) {
        return std::unexpected(
            failure(errc::invalid_argument, "repository identifier is empty", "repository"));
    }
    auto id = hash(algorithm::blake3, string_bytes(text));
    if (!id) {
        return std::unexpected(id.error());
    }
    const auto encoded = to_string(*id);
    const auto separator = encoded.find(':');
    return "repositories/" + encoded.substr(separator + 1);
}

// Where a repository's own state lives (liquid #326): keys under a prefix in a store backend.
// A repository with its own store keeps LIQUID_REPO and LIQUID_JOURNAL beside its working tree,
// through a disk backend rooted at the repository path, so that layout is unchanged. A repository
// opened on a shared store keeps them under repositories/<id-hash>/ in that store's own backend,
// so a backend with no local root holds every byte of the repository. Data is the store's
// backend: its barrier orders pack data before the marker that names it.
struct repository_state_location {
    std::shared_ptr<store_backend> backend;
    std::shared_ptr<store_backend> data;
    std::string prefix;
    std::string replication_prefix;

    [[nodiscard]] auto key(std::string_view name) const -> std::string {
        return prefix + std::string{name};
    }
    [[nodiscard]] auto child(std::string_view directory) const -> repository_state_location {
        const auto nested = prefix + std::string{directory} + "/";
        return {backend, data, nested, nested + "replication/"};
    }
};

struct prepared_repository_store {
    std::filesystem::path state_path;
    std::unique_ptr<store> owned;
    store* objects{};
    store_options configuration;
    repository_state_location state;
};

auto bytes_text(const std::vector<std::byte>& bytes) -> std::string {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Reads one state object, or nothing when it is absent.
auto read_state(const repository_state_location& state, std::string_view name)
    -> runtime::task<std::optional<std::vector<std::byte>>> {
    auto bytes = co_await store_detail::read_backend_object(state.backend, state.key(name));
    if (!bytes) {
        if (bytes.error().code == errc::not_found) {
            co_return std::optional<std::vector<std::byte>>{};
        }
        co_return std::unexpected(bytes.error());
    }
    co_return std::optional<std::vector<std::byte>>{std::move(*bytes)};
}

// Publishes one state object after everything it names: barrier on the data backend, an atomic
// replacement (or, for an exclusive create, a conditional create), then a barrier on the state
// backend so a later publication cannot overtake it.
auto publish_state(const repository_state_location& state,
                   std::string_view name,
                   std::vector<std::byte> bytes,
                   bool exclusive = false) -> runtime::task<void> {
    auto ordered = co_await state.data->barrier();
    if (!ordered) {
        co_return std::unexpected(ordered.error());
    }
    auto written = exclusive
                       ? co_await state.backend->put_if_absent(state.key(name), std::move(bytes))
                       : co_await state.backend->put(state.key(name), std::move(bytes));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return co_await state.backend->barrier();
}

// Makes everything an operation wrote durable before it returns (liquid #343): a barrier on the
// store's backend when it is a different one (retention records written after the marker), then on
// the state backend, so a removal that ends an operation is not lost in a crash. Without it a
// backend that keeps only what a barrier covered could resurrect LIQUID_JOURNAL, which open prefers
// to LIQUID_REPO, and revert an acknowledged cut; or bring back a published or discarded session.
auto settle_state(const repository_state_location& state) -> runtime::task<void> {
    if (state.data && state.data != state.backend) {
        auto data_settled = co_await state.data->barrier();
        if (!data_settled) {
            co_return std::unexpected(data_settled.error());
        }
    }
    co_return co_await state.backend->barrier();
}

// Removes one state object, then barriers, so the removal is durable when it returns.
auto remove_state(const repository_state_location& state, std::string_view name)
    -> runtime::task<void> {
    auto removed = co_await state.backend->remove(state.key(name));
    if (!removed && removed.error().code != errc::not_found) {
        co_return std::unexpected(removed.error());
    }
    co_return co_await settle_state(state);
}

// Removes every state object under a location, then barriers, so the removal is durable when it
// returns. A disk backend also removes the directory itself, as the repository did before its
// state went through the backend.
auto remove_state_tree(const repository_state_location& state) -> runtime::task<void> {
    auto directory = state.prefix;
    if (directory.ends_with('/')) {
        directory.pop_back();
    }
    if (auto local = store_detail::backend_local_path(state.backend, directory)) {
        auto code = std::error_code{};
        std::filesystem::remove_all(*local, code);
        if (code) {
            co_return std::unexpected(failure(errc::io, code.message(), local->string()));
        }
    } else {
        auto removed = co_await store_detail::remove_backend_prefix(state.backend, state.prefix);
        if (!removed) {
            co_return std::unexpected(removed.error());
        }
    }
    co_return co_await settle_state(state);
}

auto parse_state_digest(std::string text, std::string_view prefix = {}) -> result<digest> {
    text.erase(0, prefix.size());
    if (const auto newline = text.find('\n'); newline != std::string::npos) {
        text.resize(newline);
    }
    return parse_digest(text);
}

// Loads the repository document a state location publishes: the checkpoint named by LIQUID_REPO,
// replaced by the journal's document when LIQUID_JOURNAL exists (a pointer, or a legacy inline
// document).
auto load_state_document(const repository_state_location& state, store& objects)
    -> runtime::task<repository_document> {
    auto marker = co_await read_state(state, "LIQUID_REPO");
    if (!marker) {
        co_return std::unexpected(marker.error());
    }
    if (!*marker) {
        co_return std::unexpected(
            failure(errc::not_found, "repository does not exist", state.key("LIQUID_REPO")));
    }
    auto document_id = parse_state_digest(bytes_text(**marker));
    if (!document_id) {
        co_return std::unexpected(document_id.error());
    }
    auto saved = co_await objects.get(*document_id);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    auto journal = co_await read_state(state, "LIQUID_JOURNAL");
    if (!journal) {
        co_return std::unexpected(journal.error());
    }
    if (!*journal) {
        co_return decode_repository_document(*saved);
    }
    auto journal_text = bytes_text(**journal);
    if (journal_text.starts_with(journal_pointer_prefix)) {
        auto journal_id = parse_state_digest(std::move(journal_text), journal_pointer_prefix);
        if (!journal_id) {
            co_return std::unexpected(journal_id.error());
        }
        auto journal_document = co_await objects.get(*journal_id);
        if (!journal_document) {
            co_return std::unexpected(journal_document.error());
        }
        co_return decode_repository_document(*journal_document);
    }
    co_return decode_repository_document(**journal);
}

auto valid_replication_source(std::string_view source) -> result<void> {
    const auto allowed = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-' ||
               value == '_' || value == '.';
    };
    if (source.empty() || source.size() > 255 || source.front() == '.' ||
        !std::ranges::all_of(source, allowed)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "replication source is not a safe identifier",
                                       std::string{source}));
    }
    return {};
}

auto prepare_repository_store(std::filesystem::path path, repository_options& options)
    -> runtime::task<prepared_repository_store> {
    if (options.store) {
        auto configuration = repository_detail::store_access::configuration(*options.store);
        if (!configuration) {
            co_return std::unexpected(configuration.error());
        }
        auto key = identifier_key(path);
        if (!key) {
            co_return std::unexpected(key.error());
        }
        auto root = repository_detail::store_access::root_path(*options.store);
        if (!root) {
            co_return std::unexpected(root.error());
        }
        auto backend = configuration->backend;
        if (!backend) {
            co_return std::unexpected(
                failure(errc::not_configured, "store backend is unavailable", *key));
        }
        auto state =
            repository_state_location{backend, backend, *key + "/", *key + "/replication/"};
        co_return prepared_repository_store{
            *root / *key, nullptr, options.store, std::move(*configuration), std::move(state)};
    }
    auto configuration = options.storage;
    auto opened = co_await store::open(path / ".liquid-store", std::move(options.storage));
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto owned = std::make_unique<store>(std::move(*opened));
    auto* objects = owned.get();
    auto data = repository_detail::store_access::configuration(*owned);
    if (!data) {
        co_return std::unexpected(data.error());
    }
    // The checkout's state files were always written with a data sync; the backend keeps that.
    auto state = repository_state_location{
        make_disk_store_backend(path, true), data->backend, "", ".liquid/replication/"};
    co_return prepared_repository_store{
        std::move(path), std::move(owned), objects, std::move(configuration), std::move(state)};
}
} // namespace

std::string to_string(const session_id& value) {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(value.bytes.size() * 2);
    for (const auto octet : value.bytes) {
        const auto number = std::to_integer<unsigned char>(octet);
        output.push_back(digits[number >> 4U]);
        output.push_back(digits[number & 0x0fU]);
    }
    return output;
}

result<session_id> parse_session_id(std::string_view text) {
    if (text.size() != session_id{}.bytes.size() * 2) {
        return std::unexpected(
            failure(errc::invalid_argument, "session id must contain 32 hexadecimal digits"));
    }
    const auto digit = [](char value) -> std::optional<unsigned char> {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned char>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<unsigned char>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<unsigned char>(value - 'A' + 10);
        }
        return std::nullopt;
    };
    auto output = session_id{};
    for (std::size_t index = 0; index < output.bytes.size(); ++index) {
        const auto high = digit(text[index * 2]);
        const auto low = digit(text[index * 2 + 1]);
        if (!high || !low) {
            return std::unexpected(
                failure(errc::invalid_argument, "session id contains a non-hexadecimal digit"));
        }
        output.bytes[index] = std::byte{static_cast<unsigned char>((*high << 4U) | *low)};
    }
    return output;
}

struct repository::impl {
    struct retained_lease {
        digest id;
        ::liquid::lease value;
    };

    static constexpr auto lease_ttl = std::chrono::days{30};

    std::filesystem::path root_path;
    repository_state_location state;
    std::unique_ptr<store> owned_objects;
    store& objects;
    repository_document current;
    std::uint64_t checkpoint_interval{64};
    std::size_t journal_bytes{};
    algorithm hash_algorithm;
    std::uint64_t directory_load_count{};
    session_id session;
    std::map<node_id, crdt::directory_document> directory_cache;
    std::optional<retained_lease> working_tree_lease;
    std::optional<retained_lease> checkpoint_lease;
    std::optional<retained_lease> cache_root_lease;
    std::map<node_id, retained_lease> directory_leases;
    signer signing_key;
    // The signing key whose identity chain was last recorded in `current.metadata`.
    std::string recorded_chain_key;
    std::shared_ptr<keyring> keys;
    cache_report cache_statistics;
    std::shared_ptr<flow_detail::controller> flow_limits;
    std::optional<actor> acting_actor;
    std::optional<digest> acting_delegation;
    lease_authorizer authorize_lease;
    lock_authorizer authorize_lock;
    epoch_acknowledger acknowledge_epoch;
    bool defer_persistence{};
    bool suppress_drips{};
    std::uint64_t suppressed_drips{};
    std::vector<liquid::presence> transient_presence;
    std::map<std::string, std::pair<cache_status, std::optional<cache_miss>>> cache_lookups;
    impl(std::filesystem::path path,
         repository_state_location state_location,
         std::unique_ptr<store> owned,
         store& value,
         repository_document document,
         signer key,
         std::shared_ptr<keyring> encryption_keys,
         std::uint64_t interval,
         algorithm algorithm_value,
         session_id session_value)
        : root_path(std::move(path)), state(std::move(state_location)),
          owned_objects(std::move(owned)), objects(value), current(std::move(document)),
          checkpoint_interval(std::max(std::uint64_t{1}, interval)),
          hash_algorithm(algorithm_value), session(session_value), signing_key(std::move(key)),
          keys(std::move(encryption_keys)),
          flow_limits(flow_detail::shared_controller(root_path.generic_string(), current.flow)) {}
    // A handle's leases protect what it is using; when the handle ends they are released, so a
    // checkout opened and closed a thousand times does not keep a thousand sets (liquid #527).
    ~impl() {
        const auto release = [](std::optional<retained_lease>& retained) {
            if (retained)
                repository_detail::store_access::release_now(retained->value);
        };
        release(working_tree_lease);
        release(checkpoint_lease);
        release(cache_root_lease);
        for (auto& [ignored, retained] : directory_leases)
            repository_detail::store_access::release_now(retained.value);
    }
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;
    auto lease_owner(std::string_view kind) const -> std::string {
        constexpr auto digits = std::string_view{"0123456789abcdef"};
        auto output = std::string{"repository:"};
        output.reserve(output.size() + current.repository.size() * 2 + kind.size() + 1);
        for (const auto value : current.repository) {
            const auto byte = std::to_integer<unsigned char>(value);
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        }
        output.push_back('\n');
        output.append(kind);
        return output;
    }
    // The owner of a lease this handle holds for itself: its kind and this process as holder, so
    // a later open can tell a lease whose process ended (liquid #527).
    auto held_owner(std::string_view kind) const -> std::string {
        return lease_owner(kind) + "\n" + store_detail::process_holder();
    }
    // Releases this repository's leases whose holder process has ended on this host. They are what
    // a killed or crashed handle left; the published state is leased again by the open that calls
    // this, which has already checkpointed.
    runtime::task<std::uint64_t> collect_ended_holders() {
        auto store_root = repository_detail::store_access::root_path(objects);
        if (!store_root) {
            co_return std::unexpected(store_root.error());
        }
        auto store_configuration = repository_detail::store_access::configuration(objects);
        if (!store_configuration) {
            co_return std::unexpected(store_configuration.error());
        }
        const auto backend = store_configuration->backend;
        if (!backend) {
            co_return std::unexpected(failure(
                errc::not_configured, "store backend is unavailable", store_root->string()));
        }
        auto existing = co_await store_detail::load_leases(backend, *store_root / "leases");
        if (!existing) {
            co_return std::unexpected(existing.error());
        }
        const auto scope = lease_owner("");
        auto collected = std::uint64_t{};
        for (const auto& record : *existing) {
            if (!record.owner.starts_with(scope) ||
                store_detail::lease_holder_state(record.owner) !=
                    store_detail::holder_state::dead) {
                continue;
            }
            auto state = std::make_shared<store_detail::lease_state>();
            state->backend = backend;
            state->record = record;
            auto released = co_await store_detail::release_lease(std::move(state));
            if (!released) {
                co_return std::unexpected(released.error());
            }
            ++collected;
        }
        co_return collected;
    }
    runtime::task<void> restore_paged_tree(working_entry& destination, std::string path = {}) {
        auto bytes = co_await objects.get(destination.object);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto tree = objects::decode_tree(*bytes);
        if (!tree) {
            co_return std::unexpected(tree.error());
        }
        for (const auto& entry : tree->entries) {
            const auto child_path = path.empty() ? entry.name : path + "/" + entry.name;
            auto kind = entry_kind::file;
            auto reference = entry.ref;
            auto stored_kind = entry.kind;
            if (entry.kind == node_kind::conflict) {
                const auto conflict = std::ranges::find(
                    current.conflict_values, child_path, &conflict_reference::path);
                if (conflict == current.conflict_values.end()) {
                    co_return std::unexpected(
                        failure(errc::corrupt, "conflict selection is absent", child_path));
                }
                reference = conflict->selected;
                stored_kind = conflict->selected_kind;
            }
            if (stored_kind == node_kind::tree) {
                kind = entry_kind::directory;
            } else if (stored_kind == node_kind::repo) {
                kind = entry_kind::nested_repository;
            } else if (stored_kind != node_kind::blob) {
                co_return std::unexpected(
                    failure(errc::corrupt, "invalid repository tree entry", entry.name));
            }
            auto child = working_entry{kind, entry.node, entry.mode, reference, {}, entry.nested};
            if (kind == entry_kind::nested_repository) {
                const auto retained = current.nested_entries.find(to_string(child.object));
                if (retained != current.nested_entries.end()) {
                    child.entries = retained->second.entries;
                }
            }
            if (kind == entry_kind::directory) {
                auto restored = co_await restore_paged_tree(child, child_path);
                if (!restored) {
                    co_return std::unexpected(restored.error());
                }
            }
            destination.entries.emplace(entry.name, std::move(child));
        }
        co_return result<void>{};
    }
    runtime::task<void> load_paged_root() {
        if (!current.root_is_paged) {
            co_return result<void>{};
        }
        auto restored = co_await restore_paged_tree(current.root);
        if (!restored) {
            co_return std::unexpected(restored.error());
        }
        current.root_is_paged = false;
        co_return result<void>{};
    }
    runtime::task<std::optional<tree_entry>> paged_entry_at(const std::vector<std::string>& path) {
        auto root_id = current.root.object;
        for (auto index = std::size_t{}; index != path.size(); ++index) {
            auto bytes = co_await objects.get(root_id);
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            auto tree = objects::decode_tree(*bytes);
            if (!tree) {
                co_return std::unexpected(tree.error());
            }
            const auto found = std::ranges::find(tree->entries, path[index], &tree_entry::name);
            if (found == tree->entries.end()) {
                co_return std::optional<tree_entry>{};
            }
            if (index + 1 == path.size()) {
                co_return std::optional<tree_entry>{*found};
            }
            if (found->kind != node_kind::tree) {
                co_return std::optional<tree_entry>{};
            }
            root_id = found->id;
        }
        co_return std::optional<tree_entry>{};
    }
    runtime::task<void> restore_directory_document(node_id id, crdt::directory_document& document) {
        auto* working_directory = directory_with_node(current.root, id);
        if (!working_directory) {
            co_return std::unexpected(failure(
                errc::corrupt, "directory is missing from working tree", "repository document"));
        }
        auto actual = document.entries();
        if (!actual) {
            co_return std::unexpected(actual.error());
        }
        for (const auto& [name, child] : working_directory->entries) {
            const auto expected = directory_entry(child);
            auto existing = document.entry_at(name);
            if (!existing) {
                co_return std::unexpected(existing.error());
            }
            if (*existing && **existing == expected) {
                continue;
            }
            if (*existing) {
                auto removed = document.remove_entry(name);
                if (!removed) {
                    co_return std::unexpected(removed.error());
                }
            }
            auto inserted = document.put_entry(name, expected);
            if (!inserted) {
                co_return std::unexpected(inserted.error());
            }
            auto verified = document.entry_at(name);
            if (!verified) {
                co_return std::unexpected(verified.error());
            }
            if (!*verified || **verified != expected) {
                co_return std::unexpected(failure(
                    errc::corrupt, "directory document did not retain its working entry", name));
            }
        }
        for (const auto& entry : *actual) {
            if (working_directory->entries.contains(entry.name)) {
                continue;
            }
            auto removed = document.remove_entry(entry.name);
            if (!removed) {
                co_return std::unexpected(removed.error());
            }
        }
        co_return result<void>{};
    }
    runtime::task<crdt::directory_document*> directory(node_id id) {
        auto found = directory_cache.find(id);
        if (found != directory_cache.end()) {
            auto restored = co_await restore_directory_document(id, found->second);
            if (!restored) {
                co_return std::unexpected(restored.error());
            }
            co_return &found->second;
        }
        if (auto saved = current.dirs.find(id); saved != current.dirs.end()) {
            auto raw = co_await objects.get(saved->second);
            if (!raw)
                co_return std::unexpected(raw.error());
            auto loaded =
                crdt::directory_document::load_sync(*raw, "local", root_path.generic_string());
            if (!loaded) {
                auto problem = loaded.error();
                problem.context = "load active directory: " + problem.context;
                co_return std::unexpected(std::move(problem));
            }
            auto inserted = directory_cache.emplace(id, std::move(*loaded));
            ++directory_load_count;
            auto restored = co_await restore_directory_document(id, inserted.first->second);
            if (!restored)
                co_return std::unexpected(restored.error());
            co_return &inserted.first->second;
        }
        crdt::repo_id repository{};
        std::ranges::copy(current.repository, repository.begin());
        auto created =
            crdt::directory_document::create(repository, "local", root_path.generic_string());
        if (!created)
            co_return std::unexpected(created.error());
        auto inserted = directory_cache.emplace(id, std::move(*created));
        auto restored = co_await restore_directory_document(id, inserted.first->second);
        if (!restored)
            co_return std::unexpected(restored.error());
        co_return &inserted.first->second;
    }
    runtime::task<std::vector<document_heads>> capture_document_heads() {
        auto nodes = std::vector<node_id>{};
        append_directory_nodes(current.root, nodes);
        auto output = std::vector<document_heads>{};
        output.reserve(nodes.size());
        for (const auto& node : nodes) {
            auto value = co_await directory(node);
            if (!value) {
                co_return std::unexpected(value.error());
            }
            auto heads = (*value)->heads();
            if (!heads) {
                co_return std::unexpected(heads.error());
            }
            output.push_back({node, std::move(*heads)});
        }
        co_return output;
    }
    runtime::task<void>
    import_composition(impl& source, const working_entry& root, std::optional<change_id> head) {
        auto roots = std::vector<digest>{};
        append_composition_roots(root, roots);
        if (head) {
            roots.push_back(head->value);
        }
        for (const auto& [node, archive] : source.current.dirs) {
            static_cast<void>(node);
            roots.push_back(archive);
        }
        auto copied = co_await copy_composition_objects(source.objects, objects, std::move(roots));
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
        for (const auto& [node, archive] : source.current.dirs) {
            current.dirs.try_emplace(node, archive);
        }
        for (const auto& [key, value] : source.current.metadata) {
            if (key.starts_with(composition_origin_prefix)) {
                current.metadata.insert_or_assign(key, value);
            }
        }
        current.metadata.try_emplace(std::string{composition_origin_prefix} +
                                     node_text(source.current.repository));
        co_return result<void>{};
    }
    runtime::task<digest> persist(bool synchronize_reachable, bool exclusive_create = false) {
        if (synchronize_reachable) {
            auto reachable = std::set<node_id>{};
            append_working_directories(current.root, reachable);
            for (const auto& node : reachable) {
                auto synchronized = co_await directory(node);
                if (!synchronized) {
                    co_return std::unexpected(synchronized.error());
                }
            }
        }
        for (auto& [node, document] : directory_cache) {
            auto saved_document = document.save();
            if (!saved_document)
                co_return std::unexpected(saved_document.error());
            auto reopened = crdt::directory_document::load_sync(
                *saved_document, "local", root_path.generic_string());
            if (!reopened) {
                saved_document = document.save_full();
                if (!saved_document) {
                    co_return std::unexpected(saved_document.error());
                }
                reopened = crdt::directory_document::load_sync(
                    *saved_document, "local", root_path.generic_string());
                if (!reopened) {
                    auto problem = reopened.error();
                    problem.context = "reopen persisted directory: " + problem.context;
                    co_return std::unexpected(std::move(problem));
                }
            }
            auto saved_id = co_await repository_detail::store_access::put_plain(
                objects, std::move(*saved_document));
            if (!saved_id)
                co_return std::unexpected(saved_id.error());
            current.dirs[node] = *saved_id;
        }
        auto encoded = encode_repository_document(current);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        auto stored =
            co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
        if (!stored)
            co_return std::unexpected(stored.error());
        auto published = co_await publish_state(
            state, "LIQUID_REPO", string_bytes(to_string(*stored) + "\n"), exclusive_create);
        if (!published) {
            if (exclusive_create && published.error().code == errc::already_exists) {
                co_return std::unexpected(
                    failure(errc::already_exists, "repository already exists", root_path.string()));
            }
            co_return std::unexpected(published.error());
        }
        co_return *stored;
    }
    runtime::task<void> retain(std::optional<retained_lease>& retained, digest id) {
        if (retained && retained->id.alg == id.alg && retained->id.bytes == id.bytes) {
            co_return co_await retained->value.renew(lease_ttl);
        }
        auto replacement = co_await repository_detail::store_access::lease_root(
            objects, id, lease_ttl, held_owner("checkpoint"));
        if (!replacement) {
            co_return std::unexpected(replacement.error());
        }
        if (retained) {
            auto released = co_await retained->value.release();
            if (!released) {
                co_return std::unexpected(released.error());
            }
        }
        retained.emplace(retained_lease{id, std::move(*replacement)});
        co_return result<void>{};
    }
    runtime::task<void> sync_retention_roots(std::string owner_prefix,
                                             std::vector<repository_detail::retention_root> roots) {
        auto store_root = repository_detail::store_access::root_path(objects);
        if (!store_root) {
            co_return std::unexpected(store_root.error());
        }
        const auto directory = *store_root / "leases";
        auto store_configuration = repository_detail::store_access::configuration(objects);
        if (!store_configuration) {
            co_return std::unexpected(store_configuration.error());
        }
        const auto backend = store_configuration->backend;
        if (!backend) {
            co_return std::unexpected(
                failure(errc::not_configured, "store backend is unavailable", directory.string()));
        }
        const auto scoped_prefix = lease_owner(owner_prefix);
        auto existing = co_await store_detail::load_leases(backend, directory);
        if (!existing) {
            co_return std::unexpected(existing.error());
        }
        auto retained_ids = std::set<std::string>{};
        for (auto& desired : roots) {
            desired.owner = lease_owner(desired.owner);
            const auto expiry = desired.expiry.value_or(std::chrono::sys_seconds::max());
            const auto matching = std::ranges::find_if(*existing, [&](const auto& record) {
                return record.owner == desired.owner && record.root.alg == desired.root.alg &&
                       record.root.bytes == desired.root.bytes && record.expiry == expiry;
            });
            if (matching != existing->end()) {
                retained_ids.insert(matching->id);
                continue;
            }
            auto created = co_await store_detail::create_lease_until(
                backend, directory, desired.root, desired.expiry, desired.owner);
            if (!created) {
                co_return std::unexpected(created.error());
            }
        }
        for (const auto& record : *existing) {
            if (!record.owner.starts_with(scoped_prefix) || retained_ids.contains(record.id)) {
                continue;
            }
            auto state = std::make_shared<store_detail::lease_state>();
            state->backend = backend;
            state->record = record;
            auto released = co_await store_detail::release_lease(std::move(state));
            if (!released) {
                co_return std::unexpected(released.error());
            }
        }
        co_return result<void>{};
    }
    runtime::task<void> retain_directory(const node_id& node, digest id) {
        auto existing = directory_leases.find(node);
        if (existing != directory_leases.end() && existing->second.id.alg == id.alg &&
            existing->second.id.bytes == id.bytes) {
            co_return co_await existing->second.value.renew(lease_ttl);
        }
        auto replacement = co_await repository_detail::store_access::lease_root(
            objects, id, lease_ttl, held_owner("directory"));
        if (!replacement) {
            co_return std::unexpected(replacement.error());
        }
        if (existing != directory_leases.end()) {
            auto released = co_await existing->second.value.release();
            if (!released) {
                co_return std::unexpected(released.error());
            }
            existing->second = retained_lease{id, std::move(*replacement)};
        } else {
            directory_leases.emplace(node, retained_lease{id, std::move(*replacement)});
        }
        co_return result<void>{};
    }
    runtime::task<void> retain_checkpoint_roots(digest root, digest document) {
        auto retained_tree = co_await retain(working_tree_lease, root);
        if (!retained_tree) {
            co_return std::unexpected(retained_tree.error());
        }
        auto retained_document = co_await retain(checkpoint_lease, document);
        if (!retained_document) {
            co_return std::unexpected(retained_document.error());
        }
        auto ref_roots = std::vector<repository_detail::retention_root>{};
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        for (const auto& [name, head] : current.refs) {
            for (const auto& configured : current.retention.rules) {
                const auto* rule = std::get_if<ref_retention_rule>(&configured);
                if (!rule || !glob_matches(rule->glob, name)) {
                    continue;
                }
                auto expiry = std::optional<std::chrono::sys_seconds>{};
                if (rule->duration) {
                    if (*rule->duration > std::chrono::sys_seconds::max() - now) {
                        co_return std::unexpected(failure(errc::invalid_argument,
                                                          "ref retention duration is too large",
                                                          rule->name));
                    }
                    expiry = now + *rule->duration;
                }
                ref_roots.push_back(
                    {"retention:ref\n" + rule->name + "\n" + name, head.value, expiry});
                break;
            }
        }
        auto retained_refs = co_await sync_retention_roots("retention:ref\n", std::move(ref_roots));
        if (!retained_refs) {
            co_return std::unexpected(retained_refs.error());
        }
        for (const auto& [node, directory] : current.dirs) {
            auto retained_directory = co_await retain_directory(node, directory);
            if (!retained_directory) {
                co_return std::unexpected(retained_directory.error());
            }
        }
        for (auto found = directory_leases.begin(); found != directory_leases.end();) {
            if (current.dirs.contains(found->first)) {
                ++found;
                continue;
            }
            auto released = co_await found->second.value.release();
            if (!released) {
                co_return std::unexpected(released.error());
            }
            found = directory_leases.erase(found);
        }
        co_return result<void>{};
    }
    runtime::task<void> checkpoint(std::optional<digest> root = {}, bool exclusive_create = false) {
        const auto synchronize_reachable = current.pending_drips != 0;
        auto working_tree = root;
        if (!working_tree) {
            if (current.root_is_paged && current.root.entries.empty()) {
                working_tree = current.root.object;
            } else {
                auto snapshot_id = co_await snapshot(current.root);
                if (!snapshot_id) {
                    co_return std::unexpected(snapshot_id.error());
                }
                working_tree = *snapshot_id;
            }
        }
        current.pending_drips = 0;
        journal_bytes = 0;
        auto document = co_await persist(synchronize_reachable, exclusive_create);
        if (!document) {
            co_return std::unexpected(document.error());
        }
        auto retained = co_await retain_checkpoint_roots(*working_tree, *document);
        if (!retained) {
            co_return std::unexpected(retained.error());
        }
        co_return co_await remove_state(state, "LIQUID_JOURNAL");
    }
    // Records the local signer's identity chain with the repository the first time each key
    // signs here (liquid#383), so the chain can travel with these drips later. A signer without a
    // stored chain records nothing; receivers then refuse its drips as an unknown signer.
    void record_signer_chain() {
        const auto identity = signing_key.identity();
        if (identity.taap_id.empty() || recorded_chain_key == identity.key_fingerprint) {
            return;
        }
        auto chain = load_identity_chain(identity.taap_id);
        if (!chain) {
            return;
        }
        remember_identity_chain(current.metadata, identity.taap_id, *chain);
        recorded_chain_key = identity.key_fingerprint;
    }
    runtime::task<void> checkpoint_or_journal() {
        if (defer_persistence) {
            co_return result<void>{};
        }
        if (current.pending_drips >= checkpoint_interval ||
            journal_bytes >= build_detail::repository_journal_byte_budget) {
            co_return co_await checkpoint();
        }
        auto root = result<digest>{current.root.object};
        if (!current.root_is_paged || !current.root.entries.empty()) {
            root = co_await snapshot(current.root);
            if (!root) {
                co_return std::unexpected(root.error());
            }
        }
        auto document = co_await persist(false);
        if (!document) {
            co_return std::unexpected(document.error());
        }
        co_return co_await publish_state(
            state,
            "LIQUID_JOURNAL",
            string_bytes(std::string{journal_pointer_prefix} + to_string(*document) + "\n"));
    }
    runtime::task<void> append_drip(std::string operation,
                                    const node_id& node,
                                    std::chrono::sys_seconds at,
                                    std::vector<std::string> paths = {},
                                    const write_options& options = {},
                                    std::optional<stale_revalidation> revalidated = std::nullopt) {
        if (suppress_drips) {
            ++suppressed_drips;
            co_return result<void>{};
        }
        auto path_values = cbor::value::array{};
        for (auto& path : paths) {
            path_values.push_back(cbor::value{std::move(path)});
        }
        auto fields = cbor::value::map{
            {"v", cbor::value{std::uint64_t{acting_delegation ? 3U : 2U}}},
            {"kind", "drip"},
            {"author", cbor::value{signing_key.identity().taap_id}},
            {"at", cbor::value{at.time_since_epoch().count()}},
            {"node", bytes(node)},
            {"operation", cbor::value{std::move(operation)}},
            {"base", current.head ? digest_value(current.head->value) : cbor::value{cbor::null{}}},
            {"paths", cbor::value{std::move(path_values)}},
        };
        if (options.base) {
            fields.emplace_back(
                "reasoned_from",
                map({{"change", digest_value(options.base->change.value)},
                     {"cursor",
                      options.base->cursor ? cbor::value{options.base->cursor->token}
                                           : cbor::value{cbor::null{}}}}));
        }
        if (revalidated) {
            fields.emplace_back(
                "revalidated",
                cbor::value{*revalidated == stale_revalidation::disjoint ? "disjoint" : "forced"});
        }
        if (acting_delegation) {
            fields.emplace_back("delegation", digest_value(*acting_delegation));
            fields.emplace_back("agent", actor_value(current_actor()));
        }
        auto body = cbor::encode(cbor::value{std::move(fields)});
        if (!body) {
            co_return std::unexpected(body.error());
        }
        auto signed_value = co_await signing_key.sign("drip", *body);
        if (!signed_value) {
            co_return std::unexpected(signed_value.error());
        }
        auto encoded_signature = identity_detail::canonical_signature(*signed_value);
        if (!encoded_signature) {
            co_return std::unexpected(encoded_signature.error());
        }
        current.drips.push_back(
            {signing_key.identity(), std::move(*body), std::move(*encoded_signature)});
        record_signer_chain();
        journal_bytes += current.drips.back().body.size();
        journal_bytes += current.drips.back().signature.size();
        co_return result<void>{};
    }
    runtime::task<void> append_lock_drip(std::string operation,
                                         std::optional<exclusive_lock> value,
                                         std::optional<lock_config> rules = std::nullopt) {
        if (suppress_drips) {
            ++suppressed_drips;
            co_return result<void>{};
        }
        const auto at =
            value ? value->since
                  : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto paths = cbor::value::array{};
        if (value) {
            paths.push_back(cbor::value{value->path});
        }
        auto body = cbor::encode(map(
            {{"v", cbor::value{std::uint64_t{4}}},
             {"kind", "drip"},
             {"author", cbor::value{signing_key.identity().taap_id}},
             {"at", cbor::value{at.time_since_epoch().count()}},
             {"node", bytes(current.root.node)},
             {"operation", cbor::value{std::move(operation)}},
             {"base", current.head ? digest_value(current.head->value) : cbor::value{cbor::null{}}},
             {"paths", cbor::value{std::move(paths)}},
             {"lock", value ? lock_value(*value) : cbor::value{cbor::null{}}},
             {"lock_rules", rules ? lock_config_value(*rules) : cbor::value{cbor::null{}}}}));
        if (!body) {
            co_return std::unexpected(body.error());
        }
        auto signed_value = co_await signing_key.sign("drip", *body);
        if (!signed_value) {
            co_return std::unexpected(signed_value.error());
        }
        auto encoded_signature = identity_detail::canonical_signature(*signed_value);
        if (!encoded_signature) {
            co_return std::unexpected(encoded_signature.error());
        }
        current.drips.push_back(
            {signing_key.identity(), std::move(*body), std::move(*encoded_signature)});
        record_signer_chain();
        journal_bytes += current.drips.back().body.size();
        journal_bytes += current.drips.back().signature.size();
        co_return result<void>{};
    }
    runtime::task<void>
    append_redaction(std::string path, const node_id& node, const digest& target) {
        const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto body = cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                                      {"kind", "redaction"},
                                      {"author", cbor::value{signing_key.identity().taap_id}},
                                      {"at", cbor::value{at.time_since_epoch().count()}},
                                      {"node", bytes(node)},
                                      {"path", cbor::value{std::move(path)}},
                                      {"target", digest_value(target)}}));
        if (!body) {
            co_return std::unexpected(body.error());
        }
        auto signed_value = co_await signing_key.sign("drip", *body);
        if (!signed_value) {
            co_return std::unexpected(signed_value.error());
        }
        auto encoded_signature = identity_detail::canonical_signature(*signed_value);
        if (!encoded_signature) {
            co_return std::unexpected(encoded_signature.error());
        }
        current.drips.push_back(
            {signing_key.identity(), std::move(*body), std::move(*encoded_signature)});
        record_signer_chain();
        co_return result<void>{};
    }
    void record_operation(op_id id,
                          std::string kind,
                          std::string from,
                          std::string to,
                          node_id node,
                          std::optional<working_entry> prior) {
        auto operation = stored_operation{};
        operation.id = std::move(id);
        operation.kind = std::move(kind);
        operation.from = std::move(from);
        operation.to = std::move(to);
        operation.node = node;
        operation.prior = prior;
        operation.session = session;
        operation.by = current_actor();
        operation.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        operation.agent = acting_delegation.has_value();
        if (operation.kind == "write" || operation.kind == "mkdir") {
            auto parts = split_path(operation.from);
            auto* after = parts ? lookup(current.root, *parts) : nullptr;
            operation.entries.push_back(
                {operation.from, std::move(prior), after ? std::optional{*after} : std::nullopt});
        } else if (operation.kind == "remove") {
            operation.entries.push_back({operation.from, std::move(prior), std::nullopt});
        } else if (operation.kind == "move") {
            auto destination = split_path(operation.to);
            auto* after = destination ? lookup(current.root, *destination) : nullptr;
            operation.entries.push_back(
                {operation.from, after ? std::optional{*after} : std::nullopt, std::nullopt});
            operation.entries.push_back(
                {operation.to, std::nullopt, after ? std::optional{*after} : std::nullopt});
        }
        current.operations.push_back(std::move(operation));
    }
    void record_operation(stored_operation operation) {
        operation.session = session;
        operation.by = signing_key.identity();
        operation.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        operation.agent = acting_delegation.has_value();
        current.operations.push_back(std::move(operation));
    }
    runtime::task<undo_report> apply_undo(std::vector<stored_operation> operations,
                                          undo_options options) {
        auto planned = plan_undo(
            current, operations, options, hash_algorithm, session, signing_key.identity());
        if (!planned) {
            co_return std::unexpected(planned.error());
        }
        if (planned->report.applied.empty()) {
            co_return planned->report;
        }
        auto changes =
            crdt_inverse_operations(current.root, planned->document.root, planned->changed_paths);
        if (!changes) {
            co_return std::unexpected(changes.error());
        }
        auto candidates = std::map<node_id, crdt::directory_document>{};
        const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto time =
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
        for (auto& [node, node_operations] : *changes) {
            auto active = co_await directory(node);
            if (!active) {
                co_return std::unexpected(active.error());
            }
            auto candidate = (*active)->fork();
            if (!candidate) {
                co_return std::unexpected(candidate.error());
            }
            auto applied = candidate->transact(
                std::move(node_operations), crdt::commit_options{.message = "undo", .time = time});
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            candidates.emplace(node, std::move(*candidate));
        }
        auto previous = current;
        current = std::move(planned->document);
        for (auto& [node, candidate] : candidates) {
            auto active = directory_cache.find(node);
            if (active == directory_cache.end()) {
                current = std::move(previous);
                co_return std::unexpected(
                    failure(errc::internal, "undo directory cache entry disappeared"));
            }
            std::swap(active->second, candidate);
        }
        const auto rollback = [&]() {
            for (auto& [node, candidate] : candidates) {
                std::swap(directory_cache.find(node)->second, candidate);
            }
            current = std::move(previous);
        };
        auto lock_drips = std::size_t{};
        auto lock_paths = std::set<std::string>{};
        for (const auto& value : previous.locks) {
            lock_paths.insert(value.path);
        }
        for (const auto& value : current.locks) {
            lock_paths.insert(value.path);
        }
        for (const auto& path : lock_paths) {
            const auto before = std::ranges::find(previous.locks, path, &exclusive_lock::path);
            const auto after = std::ranges::find(current.locks, path, &exclusive_lock::path);
            if (before != previous.locks.end() && after == current.locks.end()) {
                auto drip = co_await append_lock_drip("unlock", *before);
                if (!drip) {
                    rollback();
                    co_return std::unexpected(drip.error());
                }
                ++lock_drips;
            } else if (before == previous.locks.end() && after != current.locks.end()) {
                auto drip = co_await append_lock_drip("lock", *after);
                if (!drip) {
                    rollback();
                    co_return std::unexpected(drip.error());
                }
                ++lock_drips;
            }
        }
        for (const auto& applied_id : planned->report.applied) {
            const auto source = std::ranges::find_if(operations, [&](const auto& operation) {
                return operation_matches(operation.id, applied_id);
            });
            const auto node = source == operations.end() ? node_id{} : source->node;
            auto drip = co_await append_drip("undo", node, at, {}, {}, std::nullopt);
            if (!drip) {
                rollback();
                co_return std::unexpected(drip.error());
            }
        }
        current.pending_drips += planned->report.applied.size() + lock_drips;
        auto saved = co_await checkpoint_or_journal();
        if (!saved) {
            rollback();
            co_return std::unexpected(saved.error());
        }
        co_return planned->report;
    }
    runtime::task<digest>
    snapshot(working_entry& entry, std::string path = {}, bool include_conflicts = true) {
        ::liquid::tree value{};
        for (auto& [name, child] : entry.entries) {
            const auto child_path = path.empty() ? name : path + "/" + name;
            digest reference = child.object;
            node_kind kind = node_kind::blob;
            const auto conflicted =
                std::ranges::find(current.conflict_values, child_path, &conflict_reference::path);
            if (include_conflicts && conflicted != current.conflict_values.end() &&
                conflicted->id.alg != algorithm{}) {
                reference = conflicted->id;
                kind = node_kind::conflict;
            } else if (child.kind == entry_kind::directory) {
                auto nested = co_await snapshot(child, child_path, include_conflicts);
                if (!nested)
                    co_return std::unexpected(nested.error());
                reference = *nested;
                child.object = reference;
                kind = node_kind::tree;
            }
            if (child.kind == entry_kind::nested_repository)
                kind = node_kind::repo;
            value.entries.push_back(
                {name, child.mode, kind, reference, child.node, reference, child.nested});
        }
        auto encoded = objects::encode(value);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        auto stored =
            co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
        if (!stored)
            co_return std::unexpected(stored.error());
        entry.object = *stored;
        co_return *stored;
    }
    runtime::task<::liquid::tree> root_tree() {
        auto snapshot_id = co_await snapshot(current.root);
        if (!snapshot_id)
            co_return std::unexpected(snapshot_id.error());
        auto saved = co_await objects.get(*snapshot_id);
        if (!saved)
            co_return std::unexpected(saved.error());
        co_return objects::decode_tree(*saved);
    }
    runtime::task<digest> object_at(change_id change, const std::vector<std::string>& path) {
        auto encoded_change = co_await objects.get(change.value);
        if (!encoded_change) {
            co_return std::unexpected(encoded_change.error());
        }
        auto record = objects::decode_change(*encoded_change);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        auto current = record->root;
        for (std::size_t index = 0; index != path.size(); ++index) {
            auto encoded_tree = co_await objects.get(current);
            if (!encoded_tree) {
                co_return std::unexpected(encoded_tree.error());
            }
            auto tree = objects::decode_tree(*encoded_tree);
            if (!tree) {
                co_return std::unexpected(tree.error());
            }
            const auto entry = std::ranges::find(tree->entries, path[index], &tree_entry::name);
            if (entry == tree->entries.end()) {
                co_return std::unexpected(
                    failure(errc::not_found, "redaction path is absent from change", path[index]));
            }
            if (index + 1 == path.size()) {
                if (entry->kind != node_kind::blob) {
                    co_return std::unexpected(failure(
                        errc::invalid_argument, "redaction path is not a file", path[index]));
                }
                co_return entry->ref;
            }
            if (entry->kind != node_kind::tree) {
                co_return std::unexpected(failure(errc::invalid_argument,
                                                  "redaction path parent is not a directory",
                                                  path[index]));
            }
            current = entry->ref;
        }
        co_return std::unexpected(
            failure(errc::invalid_argument, "redaction path is empty", "repository::redact"));
    }

    runtime::task<std::vector<std::byte>> blob_contents(digest reference, std::string path) {
        auto encoded = co_await objects.get(reference);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto record = objects::decode_blob(*encoded);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        if (record->content.alg == algorithm::blake3 || record->content.alg == algorithm::sha256 ||
            record->content.alg == algorithm::blake3_keyed) {
            auto content = co_await objects.get(record->content);
            if (!content) {
                co_return std::unexpected(content.error());
            }
            if (content->size() != record->size) {
                co_return std::unexpected(
                    failure(errc::corrupt, "blob size does not match content", std::move(path)));
            }
            co_return content;
        }
        auto output = std::vector<std::byte>{};
        output.reserve(static_cast<std::size_t>(record->size));
        for (const auto& chunk : record->chunks) {
            auto content = co_await objects.get(chunk);
            if (!content) {
                co_return std::unexpected(content.error());
            }
            output.insert(output.end(), content->begin(), content->end());
        }
        if (output.size() != record->size) {
            co_return std::unexpected(
                failure(errc::corrupt, "blob size does not match content", std::move(path)));
        }
        co_return output;
    }
    runtime::task<digest> store_conflict_value(conflict value) {
        auto parts = split_path(value.region.path);
        if (!parts) {
            co_return std::unexpected(parts.error());
        }
        auto* selected = lookup(current.root, *parts);
        if (!selected) {
            co_return std::unexpected(
                failure(errc::not_found, "conflict path is absent", value.region.path));
        }
        const auto previous_selected = selected->object;
        auto selected_kind = node_kind::blob;
        if (selected->kind == entry_kind::directory) {
            selected_kind = node_kind::tree;
        } else if (selected->kind == entry_kind::nested_repository) {
            selected_kind = node_kind::repo;
        }
        if (selected_kind != node_kind::blob) {
            auto selected_root = co_await snapshot(*selected, value.region.path, false);
            if (!selected_root) {
                co_return std::unexpected(selected_root.error());
            }
            if (same_digest(value.base, previous_selected)) {
                value.base = *selected_root;
            }
            if (same_digest(value.left, previous_selected)) {
                value.left = *selected_root;
            }
            if (same_digest(value.right, previous_selected)) {
                value.right = *selected_root;
            }
        }
        auto encoded = objects::encode(value);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto stored =
            co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        auto reference = conflict_reference{.path = value.region.path,
                                            .id = *stored,
                                            .selected = selected->object,
                                            .selected_kind = selected_kind};
        std::erase_if(current.conflict_values, [&](const conflict_reference& existing) {
            return existing.path == reference.path;
        });
        current.conflict_values.push_back(std::move(reference));
        co_return *stored;
    }
    runtime::task<std::vector<std::byte>> read_at(std::string path, change_id revision) {
        auto change_bytes = co_await objects.get(revision.value);
        if (!change_bytes) {
            co_return std::unexpected(change_bytes.error());
        }
        auto saved_change = objects::decode_change(*change_bytes, hash_algorithm);
        if (!saved_change) {
            co_return std::unexpected(saved_change.error());
        }
        auto parts = split_path(path);
        if (!parts) {
            co_return std::unexpected(parts.error());
        }
        auto tree_reference = saved_change->root;
        for (std::size_t index = 0; index < parts->size(); ++index) {
            auto tree_bytes = co_await objects.get(tree_reference);
            if (!tree_bytes) {
                co_return std::unexpected(tree_bytes.error());
            }
            auto directory = objects::decode_tree(*tree_bytes);
            if (!directory) {
                co_return std::unexpected(directory.error());
            }
            const auto found =
                std::ranges::find(directory->entries, (*parts)[index], &tree_entry::name);
            if (found == directory->entries.end()) {
                co_return std::unexpected(
                    failure(errc::not_found, "file does not exist at change", path));
            }
            if (index + 1 == parts->size()) {
                if (found->kind != node_kind::blob) {
                    co_return std::unexpected(
                        failure(errc::not_found, "path is not a file at change", path));
                }
                co_return co_await blob_contents(found->ref, std::move(path));
            }
            if (found->kind != node_kind::tree) {
                co_return std::unexpected(
                    failure(errc::not_found, "path component is not a directory", path));
            }
            tree_reference = found->ref;
        }
        co_return std::unexpected(failure(errc::not_found, "file does not exist at change", path));
    }
    runtime::task<std::vector<std::byte>>
    encrypt_json_write(std::string path,
                       std::vector<std::byte> contents,
                       encryption_path_rule rule,
                       std::optional<digest> prior_reference) {
        const auto source =
            std::string_view{reinterpret_cast<const char*>(contents.data()), contents.size()};
        auto parsed = formats::json::parse(source);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto leaf_rule = encryption_detail::leaf_rule{rule.key_id, rule.encrypted_regex};
        if (prior_reference) {
            auto prior_bytes = co_await blob_contents(*prior_reference, path);
            if (!prior_bytes) {
                co_return std::unexpected(prior_bytes.error());
            }
            const auto prior_source = std::string_view{
                reinterpret_cast<const char*>(prior_bytes->data()), prior_bytes->size()};
            auto prior_ciphertext = formats::json::parse(prior_source);
            if (!prior_ciphertext) {
                co_return std::unexpected(prior_ciphertext.error());
            }
            auto prior_plaintext = co_await encryption_detail::leaf_cipher::decrypt_json(
                *prior_ciphertext, leaf_rule, keys);
            if (!prior_plaintext) {
                co_return std::unexpected(prior_plaintext.error());
            }
            auto preserved = encryption_detail::preserve_json_envelopes(
                *parsed, *prior_plaintext, *prior_ciphertext, leaf_rule);
            if (!preserved) {
                co_return std::unexpected(preserved.error());
            }
        }
        auto encrypted = co_await encryption_detail::leaf_cipher::encrypt_json(
            std::move(*parsed), leaf_rule, keys);
        if (!encrypted) {
            co_return std::unexpected(encrypted.error());
        }
        auto written = formats::json::write(*encrypted);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        co_return string_bytes(*written);
    }
    runtime::task<std::vector<std::byte>>
    encrypt_dotenv_write(std::string path,
                         std::vector<std::byte> contents,
                         encryption_path_rule rule,
                         std::optional<digest> prior_reference) {
        const auto source =
            std::string_view{reinterpret_cast<const char*>(contents.data()), contents.size()};
        auto parsed = formats::dotenv::parse(source);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto leaf_rule = encryption_detail::leaf_rule{rule.key_id, rule.encrypted_regex};
        if (prior_reference) {
            auto prior_bytes = co_await blob_contents(*prior_reference, path);
            if (!prior_bytes) {
                co_return std::unexpected(prior_bytes.error());
            }
            const auto prior_source = std::string_view{
                reinterpret_cast<const char*>(prior_bytes->data()), prior_bytes->size()};
            auto prior_ciphertext = formats::dotenv::parse(prior_source);
            if (!prior_ciphertext) {
                co_return std::unexpected(prior_ciphertext.error());
            }
            auto prior_plaintext = co_await encryption_detail::leaf_cipher::decrypt_dotenv(
                *prior_ciphertext, leaf_rule, keys);
            if (!prior_plaintext) {
                co_return std::unexpected(prior_plaintext.error());
            }
            auto preserved = encryption_detail::preserve_dotenv_envelopes(
                *parsed, *prior_plaintext, *prior_ciphertext, leaf_rule);
            if (!preserved) {
                co_return std::unexpected(preserved.error());
            }
        }
        auto encrypted = co_await encryption_detail::leaf_cipher::encrypt_dotenv(
            std::move(*parsed), leaf_rule, keys);
        if (!encrypted) {
            co_return std::unexpected(encrypted.error());
        }
        auto written = formats::dotenv::write(*encrypted);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        co_return string_bytes(*written);
    }
    runtime::task<std::vector<std::byte>> prepare_write(std::string path,
                                                        std::vector<std::byte> contents,
                                                        std::optional<digest> prior_reference) {
        const auto* selected = encryption_rule_for(current, path);
        if (!selected) {
            co_return contents;
        }
        auto rule = *selected;
        if (rule.mode == encryption_mode::file) {
            const auto source =
                std::string_view{reinterpret_cast<const char*>(contents.data()), contents.size()};
            if (encryption_detail::is_envelope(source, rule.key_id)) {
                co_return contents;
            }
            co_return co_await encryption_detail::leaf_cipher::encrypt_file(
                std::move(contents), std::move(path), rule.key_id, keys);
        }
        const auto format = leaf_format(path);
        if (format == "json") {
            co_return co_await encrypt_json_write(
                std::move(path), std::move(contents), std::move(rule), prior_reference);
        }
        if (format == "dotenv") {
            co_return co_await encrypt_dotenv_write(
                std::move(path), std::move(contents), std::move(rule), prior_reference);
        }
        co_return std::unexpected(
            failure(errc::unsupported, "leaf encryption supports JSON and .env", path));
    }
    auto current_actor() const -> actor {
        return acting_actor ? *acting_actor : signing_key.identity();
    }
    auto matching_lock(std::string_view path) const -> const exclusive_lock* {
        const auto found = std::ranges::find_if(current.locks, [&](const auto& value) {
            return same_path_or_descendant(path, value.path);
        });
        return found == current.locks.end() ? nullptr : &*found;
    }
    auto conflicting_lock(std::string_view path) const -> const exclusive_lock* {
        const auto actor = current_actor();
        const auto found = std::ranges::find_if(current.locks, [&](const auto& value) {
            return locks_overlap(path, value.path) && value.holder.taap_id != actor.taap_id;
        });
        return found == current.locks.end() ? nullptr : &*found;
    }
    auto lock_rule_matches(std::string_view path) const -> bool {
        return std::ranges::any_of(current.lock_rules.rules, [&](const auto& rule) {
            return std::ranges::any_of(
                rule.paths, [&](const auto& pattern) { return glob_matches(pattern, path); });
        });
    }
    auto validate_lock_write(std::string_view path) const -> result<void> {
        const auto* held = matching_lock(path);
        if (!held ||
            (!held->holder.taap_id.empty() && held->holder.taap_id == current_actor().taap_id)) {
            return {};
        }
        const auto holder = held->holder.taap_id.empty() ? "unknown holder" : held->holder.taap_id;
        return std::unexpected(failure(errc::locked,
                                       std::format("path is locked by {} since {}: {}",
                                                   holder,
                                                   held->since.time_since_epoch().count(),
                                                   held->reason),
                                       held->path));
    }
    auto validate_lock_acquire(std::string_view path) const -> result<void> {
        const auto* held = conflicting_lock(path);
        if (!held) {
            return {};
        }
        const auto holder = held->holder.taap_id.empty() ? "unknown holder" : held->holder.taap_id;
        return std::unexpected(failure(errc::locked,
                                       std::format("path is locked by {} since {}: {}",
                                                   holder,
                                                   held->since.time_since_epoch().count(),
                                                   held->reason),
                                       held->path));
    }
    static auto ranges_overlap(const frontend_source_range& left,
                               const frontend_source_range& right) -> bool {
        return left.start.byte < right.end.byte && right.start.byte < left.end.byte;
    }
    static auto same_claim(const lease_claim& left, const lease_claim& right) -> bool {
        if (left.path != right.path || left.kind != right.kind || left.symbol != right.symbol ||
            left.range.has_value() != right.range.has_value()) {
            return false;
        }
        if (!left.range) {
            return true;
        }
        return left.range->start.byte == right.range->start.byte &&
               left.range->end.byte == right.range->end.byte;
    }
    auto lease_conflict(const lease_claim& requested, const semantic_lease& held) const -> bool {
        if (requested.path != held.claim.path) {
            return false;
        }
        if (requested.kind == lease_kind::file || held.claim.kind == lease_kind::file) {
            return true;
        }
        if (requested.kind == lease_kind::symbol && held.claim.kind == lease_kind::symbol &&
            requested.symbol && held.claim.symbol && *requested.symbol == *held.claim.symbol) {
            return true;
        }
        if (!requested.range || !held.claim.range) {
            return true;
        }
        return ranges_overlap(*requested.range, *held.claim.range);
    }
    auto lease_holder(const lease_claim& requested) const -> const semantic_lease* {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto& writer = current_actor();
        const auto found = std::ranges::find_if(current.leases, [&](const auto& held) {
            const auto owned = held.by.taap_id == writer.taap_id && held.session == session;
            return held.expires > now && !owned && lease_conflict(requested, held);
        });
        return found == current.leases.end() ? nullptr : &*found;
    }
    auto validate_lease_write(std::string_view path, const write_options& options) const
        -> result<void> {
        auto requested = lease_claim{.path = std::string{path},
                                     .kind = options.symbol  ? lease_kind::symbol
                                             : options.range ? lease_kind::range
                                                             : lease_kind::file,
                                     .symbol = options.symbol,
                                     .range = options.range};
        const auto* holder = lease_holder(requested);
        if (!holder) {
            return {};
        }
        return std::unexpected(
            failure(errc::locked,
                    "write intersects a semantic lease held by " + holder->by.taap_id,
                    holder->claim.path));
    }
    auto base_cursor(const write_base& base) const -> result<std::size_t> {
        if (base.cursor) {
            auto parsed = parse_sync_cursor(base.cursor->token);
            if (!parsed || parsed->drips > current.drips.size()) {
                return std::unexpected(parsed ? failure(errc::invalid_argument,
                                                        "write cursor is beyond the journal",
                                                        base.cursor->token)
                                              : parsed.error());
            }
            return parsed->drips;
        }
        for (std::size_t index = 0; index < current.drips.size(); ++index) {
            auto decoded = decode_repository_drip(current.drips[index].body);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (decoded->base && same_digest(decoded->base->value, base.change.value)) {
                return index;
            }
        }
        if (current.head && same_digest(current.head->value, base.change.value)) {
            return current.drips.size();
        }
        return std::size_t{};
    }
    auto base_required_for_writer() const -> bool {
        const auto identity = signing_key.identity().taap_id;
        return current.agents.require_base &&
               std::ranges::find(current.agents.actors, identity) != current.agents.actors.end();
    }
    runtime::task<void> record_blob(std::string path,
                                    ::liquid::blob record,
                                    write_options options,
                                    std::optional<stale_revalidation> revalidated,
                                    std::optional<node_id> requested_node = std::nullopt,
                                    std::uint64_t update_bytes = 0) {
        auto leased = validate_lease_write(path, options);
        if (!leased) {
            co_return std::unexpected(leased.error());
        }
        auto lock_allowed = validate_lock_write(path);
        if (!lock_allowed) {
            co_return std::unexpected(lock_allowed.error());
        }
        auto parts = split_path(path);
        if (!parts)
            co_return std::unexpected(parts.error());
        auto* bucket_parent =
            parent_for(current.root, *parts, false, hash_algorithm).value_or(&current.root);
        auto admitted = flow_limits->admit(current_actor().taap_id,
                                           flow_detail::node_key(bucket_parent->node),
                                           std::max<std::uint64_t>(update_bytes, path.size() + 64));
        if (!admitted)
            co_return std::unexpected(admitted.error());
        if (!record.remote)
            record.content = {};
        auto encoded = objects::encode(record);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        auto reference =
            co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
        if (!reference)
            co_return std::unexpected(reference.error());
        auto parent = parent_for(current.root, *parts, true, hash_algorithm);
        if (!parent)
            co_return std::unexpected(parent.error());
        auto found = (*parent)->entries.find(parts->back());
        std::optional<working_entry> prior;
        if (found != (*parent)->entries.end() && found->second.kind != entry_kind::file) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "cannot overwrite directory", path));
        }
        auto directory_document = co_await directory((*parent)->node);
        if (!directory_document)
            co_return std::unexpected(directory_document.error());
        const auto entry_node = found == (*parent)->entries.end()
                                    ? requested_node.value_or(random_id())
                                    : found->second.node;
        const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto time =
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
        auto recorded = (*directory_document)
                            ->put_entry(parts->back(),
                                        {"blob", entry_node, options.mode, to_string(*reference)},
                                        crdt::commit_options{.message = "write", .time = time});
        if (!recorded)
            co_return std::unexpected(recorded.error());
        if (found != (*parent)->entries.end()) {
            prior = found->second;
            found->second.object = *reference;
            found->second.mode = options.mode;
        } else {
            found =
                (*parent)
                    ->entries
                    .emplace(parts->back(),
                             working_entry{
                                 entry_kind::file, entry_node, options.mode, *reference, {}, {}})
                    .first;
        }
        const auto conflict =
            std::ranges::find(current.conflict_values, path, &conflict_reference::path);
        if (conflict != current.conflict_values.end()) {
            conflict->selected = *reference;
            conflict->selected_kind = node_kind::blob;
        }
        auto operation = random_operation_id(hash_algorithm);
        if (!operation)
            co_return std::unexpected(operation.error());
        record_operation(*operation, "write", path, {}, found->second.node, std::move(prior));
        auto signed_drip =
            co_await append_drip("write", found->second.node, at, {path}, options, revalidated);
        if (!signed_drip)
            co_return std::unexpected(signed_drip.error());
        ++current.pending_drips;
        auto saved = co_await checkpoint_or_journal();
        if (!saved)
            co_return std::unexpected(saved.error());
        co_return result<void>{};
    }
    auto ignored(std::string_view path, const import_options& options) const -> bool {
        // Checkout-local state is never repository content, and no `!` pattern re-includes it
        // (liquid #420): otherwise a pushed tree would carry release policy or a repository marker
        // that a later clone materialises.
        if (repository_detail::local_state_path(path)) {
            return true;
        }
        auto ignored = false;
        for (const auto& configured : options.ignore_patterns) {
            auto pattern = std::string_view{configured};
            const auto included = pattern.starts_with('!');
            if (included) {
                pattern.remove_prefix(1);
            }
            auto matches = glob_matches(pattern, path);
            if (pattern.ends_with("/**")) {
                auto root = pattern.substr(0, pattern.size() - 3);
                matches =
                    matches || glob_matches(root, path) || same_path_or_descendant(path, root);
            }
            if (matches) {
                ignored = !included;
            }
        }
        return ignored;
    }
    runtime::task<void> import_directory(working_entry& destination,
                                         const std::filesystem::path& directory,
                                         std::string relative,
                                         const import_options& options,
                                         tree_import_report& report,
                                         std::vector<std::string>& changed_paths) {
        auto code = std::error_code{};
        const auto flags =
            std::filesystem::directory_options::skip_permission_denied |
            (options.follow_symlinks ? std::filesystem::directory_options::follow_directory_symlink
                                     : std::filesystem::directory_options::none);
        for (std::filesystem::directory_iterator iterator{directory, flags, code};
             !code && iterator != std::filesystem::directory_iterator{};
             iterator.increment(code)) {
            const auto name = iterator->path().filename().generic_string();
            const auto path = relative.empty() ? name : relative + "/" + name;
            if (ignored(path, options)) {
                if (iterator->is_directory(code)) {
                    continue;
                }
                continue;
            }
            auto status = iterator->symlink_status(code);
            if (code) {
                break;
            }
            if (std::filesystem::is_symlink(status)) {
                if (!options.follow_symlinks) {
                    continue;
                }
                status = iterator->status(code);
                if (code) {
                    break;
                }
            }
            if (std::filesystem::is_directory(status)) {
                auto found = destination.entries.find(name);
                if (found == destination.entries.end() ||
                    found->second.kind != entry_kind::directory) {
                    if (found != destination.entries.end()) {
                        destination.entries.erase(found);
                    }
                    found = destination.entries
                                .emplace(name,
                                         working_entry{entry_kind::directory,
                                                       random_id(),
                                                       0040755,
                                                       {hash_algorithm, {}},
                                                       {},
                                                       {}})
                                .first;
                }
                auto imported = co_await import_directory(
                    found->second, iterator->path(), path, options, report, changed_paths);
                if (!imported) {
                    co_return std::unexpected(imported.error());
                }
                continue;
            }
            if (!std::filesystem::is_regular_file(status)) {
                continue;
            }
            ++report.files_seen;
            const auto size = std::filesystem::file_size(iterator->path(), code);
            if (code) {
                break;
            }
            report.bytes_hashed += size;
            auto content = co_await objects.put_file(iterator->path());
            if (!content) {
                co_return std::unexpected(content.error());
            }
            auto record = objects.blob_for(*content);
            if (!record) {
                co_return std::unexpected(record.error());
            }
            record->content = {};
            auto encoded = objects::encode(*record);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto reference =
                co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
            if (!reference) {
                co_return std::unexpected(reference.error());
            }
            const auto mode = static_cast<std::uint32_t>(status.permissions()) & 0777U;
            const auto file_mode = std::uint32_t{0100000} | mode;
            auto found = destination.entries.find(name);
            if (found != destination.entries.end() && found->second.kind == entry_kind::file &&
                same_digest(found->second.object, *reference) && found->second.mode == file_mode) {
                continue;
            }
            if (found == destination.entries.end()) {
                destination.entries.emplace(
                    name,
                    working_entry{entry_kind::file, random_id(), file_mode, *reference, {}, {}});
            } else if (found->second.kind == entry_kind::file) {
                found->second.mode = file_mode;
                found->second.object = *reference;
            } else {
                found->second =
                    working_entry{entry_kind::file, random_id(), file_mode, *reference, {}, {}};
            }
            ++report.changed;
            report.bytes_written += size;
            changed_paths.push_back(path);
        }
        if (code) {
            co_return std::unexpected(failure(errc::io, code.message(), directory.string()));
        }
        for (auto found = destination.entries.begin(); found != destination.entries.end();) {
            const auto path = relative.empty() ? found->first : relative + "/" + found->first;
            if (ignored(path, options)) {
                ++found;
                continue;
            }
            const auto disk = directory / found->first;
            auto status = std::filesystem::symlink_status(disk, code);
            if (code == std::errc::no_such_file_or_directory) {
                code.clear();
                if (found->second.kind == entry_kind::file) {
                    ++report.changed;
                    changed_paths.push_back(path);
                }
                found = destination.entries.erase(found);
                continue;
            }
            if (code) {
                co_return std::unexpected(failure(errc::io, code.message(), disk.string()));
            }
            if (std::filesystem::is_symlink(status) && !options.follow_symlinks) {
                ++found;
                continue;
            }
            ++found;
        }
        co_return result<void>{};
    }
    struct paged_import_result {
        digest root;
        bool changed{};
    };
    runtime::task<paged_import_result>
    import_paged_directory(const std::optional<digest>& previous,
                           const node_id& node,
                           std::uint32_t mode,
                           const std::filesystem::path& directory,
                           std::string relative,
                           const import_options& options,
                           tree_import_report& report,
                           std::vector<std::string>& changed_paths) {
        auto prior = ::liquid::tree{};
        if (previous) {
            auto bytes = co_await objects.get(*previous);
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            auto decoded = objects::decode_tree(*bytes);
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            prior = std::move(*decoded);
        }
        auto output = ::liquid::tree{};
        auto seen = std::set<std::string>{};
        auto changed = false;
        auto code = std::error_code{};
        const auto flags =
            std::filesystem::directory_options::skip_permission_denied |
            (options.follow_symlinks ? std::filesystem::directory_options::follow_directory_symlink
                                     : std::filesystem::directory_options::none);
        for (std::filesystem::directory_iterator iterator{directory, flags, code};
             !code && iterator != std::filesystem::directory_iterator{};
             iterator.increment(code)) {
            const auto name = iterator->path().filename().generic_string();
            const auto path = relative.empty() ? name : relative + "/" + name;
            const auto old = std::ranges::find(prior.entries, name, &tree_entry::name);
            if (ignored(path, options)) {
                if (old != prior.entries.end()) {
                    output.entries.push_back(*old);
                }
                seen.insert(name);
                continue;
            }
            auto status = iterator->symlink_status(code);
            if (code) {
                break;
            }
            if (std::filesystem::is_symlink(status)) {
                if (!options.follow_symlinks) {
                    seen.insert(name);
                    continue;
                }
                status = iterator->status(code);
                if (code) {
                    break;
                }
            }
            seen.insert(name);
            if (std::filesystem::is_directory(status)) {
                const auto old_root = old != prior.entries.end() && old->kind == node_kind::tree
                                          ? std::optional<digest>{old->id}
                                          : std::optional<digest>{};
                const auto old_node = old != prior.entries.end() && old->kind == node_kind::tree
                                          ? old->node
                                          : random_id();
                const auto directory_mode =
                    std::uint32_t{0040000} |
                    (static_cast<std::uint32_t>(status.permissions()) & std::uint32_t{0777});
                auto imported = co_await import_paged_directory(old_root,
                                                                old_node,
                                                                directory_mode,
                                                                iterator->path(),
                                                                path,
                                                                options,
                                                                report,
                                                                changed_paths);
                if (!imported) {
                    co_return std::unexpected(imported.error());
                }
                const auto same = old != prior.entries.end() && old->kind == node_kind::tree &&
                                  old->mode == directory_mode &&
                                  same_digest(old->id, imported->root);
                output.entries.push_back({name,
                                          directory_mode,
                                          node_kind::tree,
                                          imported->root,
                                          old_node,
                                          imported->root,
                                          {}});
                changed = changed || imported->changed || !same;
                continue;
            }
            if (!std::filesystem::is_regular_file(status)) {
                continue;
            }
            ++report.files_seen;
            const auto size = std::filesystem::file_size(iterator->path(), code);
            if (code) {
                break;
            }
            report.bytes_hashed += size;
            auto content = co_await objects.put_file(iterator->path());
            if (!content) {
                co_return std::unexpected(content.error());
            }
            auto record = objects.blob_for(*content);
            if (!record) {
                co_return std::unexpected(record.error());
            }
            record->content = {};
            auto encoded = objects::encode(*record);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto reference =
                co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
            if (!reference) {
                co_return std::unexpected(reference.error());
            }
            const auto file_mode =
                std::uint32_t{0100000} |
                (static_cast<std::uint32_t>(status.permissions()) & std::uint32_t{0777});
            const auto unchanged = old != prior.entries.end() && old->kind == node_kind::blob &&
                                   old->mode == file_mode && same_digest(old->id, *reference);
            const auto file_node = unchanged ? old->node : random_id();
            output.entries.push_back(
                {name, file_mode, node_kind::blob, *reference, file_node, *reference, {}});
            if (!unchanged) {
                changed = true;
                ++report.changed;
                report.bytes_written += size;
                changed_paths.push_back(path);
            }
        }
        if (code) {
            co_return std::unexpected(failure(errc::io, code.message(), directory.string()));
        }
        for (const auto& old : prior.entries) {
            if (seen.contains(old.name)) {
                continue;
            }
            const auto path = relative.empty() ? old.name : relative + "/" + old.name;
            if (ignored(path, options)) {
                output.entries.push_back(old);
                continue;
            }
            changed = true;
            ++report.changed;
            changed_paths.push_back(path);
        }
        if (!changed) {
            co_return paged_import_result{*previous, false};
        }
        auto encoded = objects::encode(output);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto stored =
            co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        co_return paged_import_result{*stored, true};
    }
};
repository::repository() noexcept = default;
repository::~repository() = default;
repository::repository(repository&&) noexcept = default;
auto repository::operator=(repository&&) noexcept -> repository& = default;
runtime::task<repository> repository::create(std::filesystem::path path,
                                             repository_options options) {
    if (options.verify.rate == 0) {
        co_return std::unexpected(failure(
            errc::invalid_argument, "verification rate must be positive", "config.verify.rate"));
    }
    auto valid_rules = validate_encryption_rules(options.encrypt.paths);
    if (!valid_rules) {
        co_return std::unexpected(valid_rules.error());
    }
    auto valid_policy = build_detail::validate_policy(options.policy);
    if (!valid_policy) {
        co_return std::unexpected(valid_policy.error());
    }
    auto valid_retention = validate_retention_rules(options.retention);
    if (!valid_retention) {
        co_return std::unexpected(valid_retention.error());
    }
    auto valid_agents = validate_agents_config(options.agents);
    if (!valid_agents) {
        co_return std::unexpected(valid_agents.error());
    }
    auto valid_data = validate_data_config(options.data);
    if (!valid_data) {
        co_return std::unexpected(valid_data.error());
    }
    auto valid_hot_update = validate_hot_update_config(options.hot_update);
    if (!valid_hot_update) {
        co_return std::unexpected(valid_hot_update.error());
    }
    auto valid_flow = flow_detail::validate(options.flow);
    if (!valid_flow) {
        co_return std::unexpected(valid_flow.error());
    }
    auto valid_locks = validate_lock_config(options.locks);
    if (!valid_locks) {
        co_return std::unexpected(valid_locks.error());
    }
    auto prepared = co_await prepare_repository_store(std::move(path), options);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    // A cheap refusal before any object is written; the conditional create of LIQUID_REPO in the
    // first checkpoint is what makes the create exclusive against a concurrent creator.
    auto existing = co_await store_detail::backend_exists(prepared->state.backend,
                                                          prepared->state.key("LIQUID_REPO"));
    if (!existing)
        co_return std::unexpected(existing.error());
    if (*existing)
        co_return std::unexpected(failure(
            errc::already_exists, "repository already exists", prepared->state_path.string()));
    const auto hash_algorithm = prepared->configuration.hash_algorithm;
    auto keys = prepared->configuration.keys;
    repository_document document{};
    document.repository = random_id();
    document.root.node = random_id();
    document.root.object.alg = hash_algorithm;
    document.encryption_rules = std::move(options.encrypt.paths);
    document.cache_publish = options.cache.publish;
    document.verification_rate = options.verify.rate;
    document.agents = std::move(options.agents);
    document.hot_update = std::move(options.hot_update);
    document.deploy = std::move(options.deploy);
    document.policy = std::move(options.policy);
    document.retention = std::move(options.retention);
    document.flow = std::move(options.flow);
    document.remote = std::move(options.remote);
    document.data = std::move(options.data);
    document.lock_rules = std::move(options.locks);
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key)
        co_return std::unexpected(signing_key.error());
    repository output;
    auto* objects = prepared->objects;
    output.impl_ = std::make_unique<impl>(std::move(prepared->state_path),
                                          std::move(prepared->state),
                                          std::move(prepared->owned),
                                          *objects,
                                          std::move(document),
                                          std::move(*signing_key),
                                          std::move(keys),
                                          options.checkpoint_interval,
                                          hash_algorithm,
                                          options.session.value_or(session_id{random_id()}));
    output.impl_->authorize_lease = std::move(options.authorize_lease);
    output.impl_->authorize_lock = std::move(options.authorize_lock);
    output.impl_->acknowledge_epoch = std::move(options.acknowledge_epoch);
    if (!output.impl_->current.lock_rules.rules.empty()) {
        auto configured = co_await output.impl_->append_lock_drip(
            "lock_config", std::nullopt, output.impl_->current.lock_rules);
        if (!configured) {
            co_return std::unexpected(configured.error());
        }
        ++output.impl_->current.pending_drips;
    }
    auto saved = co_await output.impl_->checkpoint(std::nullopt, true);
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return output;
}
runtime::task<repository> repository::open(std::filesystem::path path, repository_options options) {
    auto prepared = co_await prepare_repository_store(std::move(path), options);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    const auto hash_algorithm = prepared->configuration.hash_algorithm;
    auto keys = prepared->configuration.keys;
    auto document = co_await load_state_document(prepared->state, *prepared->objects);
    if (!document)
        co_return std::unexpected(document.error());
    auto valid_rules = validate_encryption_rules(document->encryption_rules);
    if (!valid_rules)
        co_return std::unexpected(valid_rules.error());
    auto valid_policy = build_detail::validate_policy(document->policy);
    if (!valid_policy)
        co_return std::unexpected(valid_policy.error());
    auto valid_retention = validate_retention_rules(document->retention);
    if (!valid_retention) {
        co_return std::unexpected(valid_retention.error());
    }
    auto valid_agents = validate_agents_config(document->agents);
    if (!valid_agents) {
        co_return std::unexpected(valid_agents.error());
    }
    auto valid_data = validate_data_config(document->data);
    if (!valid_data) {
        co_return std::unexpected(valid_data.error());
    }
    auto valid_hot_update = validate_hot_update_config(document->hot_update);
    if (!valid_hot_update) {
        co_return std::unexpected(valid_hot_update.error());
    }
    auto valid_flow = flow_detail::validate(document->flow);
    if (!valid_flow) {
        co_return std::unexpected(valid_flow.error());
    }
    auto valid_locks = validate_lock_config(document->lock_rules);
    if (!valid_locks) {
        co_return std::unexpected(valid_locks.error());
    }
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key)
        co_return std::unexpected(signing_key.error());
    repository output;
    auto* objects = prepared->objects;
    output.impl_ = std::make_unique<impl>(std::move(prepared->state_path),
                                          std::move(prepared->state),
                                          std::move(prepared->owned),
                                          *objects,
                                          std::move(*document),
                                          std::move(*signing_key),
                                          std::move(keys),
                                          options.checkpoint_interval,
                                          hash_algorithm,
                                          options.session.value_or(session_id{random_id()}));
    output.impl_->authorize_lease = std::move(options.authorize_lease);
    output.impl_->authorize_lock = std::move(options.authorize_lock);
    output.impl_->acknowledge_epoch = std::move(options.acknowledge_epoch);
    if (output.impl_->current.root_is_paged && !options.defer_tree_load) {
        auto restored = co_await output.impl_->load_paged_root();
        if (!restored) {
            co_return std::unexpected(restored.error());
        }
    }
    auto checkpointed = co_await output.impl_->checkpoint();
    if (!checkpointed)
        co_return std::unexpected(checkpointed.error());
    // After the checkpoint, which leased the published state for this handle, so the leases of
    // ended handles are no longer what retains it (liquid #527).
    auto collected = co_await output.impl_->collect_ended_holders();
    if (!collected)
        co_return std::unexpected(collected.error());
    co_return output;
}

runtime::task<std::uint64_t> repository::lease_count() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto store_root = repository_detail::store_access::root_path(impl_->objects);
    if (!store_root)
        co_return std::unexpected(store_root.error());
    auto store_configuration = repository_detail::store_access::configuration(impl_->objects);
    if (!store_configuration)
        co_return std::unexpected(store_configuration.error());
    if (!store_configuration->backend)
        co_return std::unexpected(
            failure(errc::not_configured, "store backend is unavailable", store_root->string()));
    auto existing =
        co_await store_detail::load_leases(store_configuration->backend, *store_root / "leases");
    if (!existing)
        co_return std::unexpected(existing.error());
    co_return static_cast<std::uint64_t>(existing->size());
}

std::optional<remote_config> repository::remote_configuration() const {
    return impl_ ? impl_->current.remote : std::nullopt;
}
hot_update_config repository::hot_update_configuration() const {
    return impl_ ? impl_->current.hot_update : hot_update_config{};
}

deployment_config repository::deploy_configuration() const {
    return impl_ ? impl_->current.deploy : deployment_config{};
}

flow_config repository::flow_configuration() const {
    return impl_ ? impl_->current.flow : flow_config{};
}

std::vector<flow_bucket_status> repository::flow_buckets() const {
    return impl_ ? impl_->flow_limits->status() : std::vector<flow_bucket_status>{};
}
runtime::task<::liquid::tree> repository::root() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await impl_->root_tree();
}
result<node_id> repository::id() const {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return impl_->current.repository;
}
runtime::task<digest> repository::root_digest() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await impl_->snapshot(impl_->current.root);
}
runtime::task<change_id> repository::head() {
    if (!impl_ || !impl_->current.head)
        co_return std::unexpected(failure(errc::not_found, "repository has no change"));
    co_return *impl_->current.head;
}
runtime::task<stale_base_report> repository::explain_stale(write_base base,
                                                           std::vector<std::string> paths) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (!impl_->current.head) {
        co_return std::unexpected(failure(errc::not_found, "repository has no change"));
    }
    const auto cursor = impl_->base_cursor(base);
    if (!cursor) {
        co_return std::unexpected(cursor.error());
    }
    for (const auto& path : paths) {
        auto parsed = split_path(path);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
    }
    auto old_snapshot = co_await at(base.change);
    if (!old_snapshot) {
        co_return std::unexpected(old_snapshot.error());
    }
    auto report = stale_base_report{.base = std::move(base),
                                    .head = *impl_->current.head,
                                    .since_drips = impl_->current.drips.size() - *cursor};
    auto changed_paths = std::set<std::string>{};
    for (auto index = *cursor; index < impl_->current.drips.size(); ++index) {
        auto decoded = decode_repository_drip(impl_->current.drips[index].body);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto& candidates = decoded->paths.empty() ? paths : decoded->paths;
        for (const auto& candidate : candidates) {
            if (paths.empty() || std::ranges::any_of(paths, [&](const auto& path) {
                    return paths_overlap(path, candidate);
                })) {
                changed_paths.insert(candidate);
            }
        }
    }
    if (changed_paths.empty() && !same_digest(report.base.change.value, report.head.value)) {
        auto differences = co_await diff(report.base.change, report.head, {.text = true});
        if (!differences) {
            co_return std::unexpected(differences.error());
        }
        for (const auto& difference : *differences) {
            if (paths.empty() || std::ranges::any_of(paths, [&](const auto& path) {
                    return paths_overlap(path, difference.path) ||
                           (difference.old_path && paths_overlap(path, *difference.old_path));
                })) {
                report.changed.push_back(
                    {difference.path, difference.line_hunks.value_or(std::vector<line_change>{})});
            }
        }
        co_return report;
    }
    for (const auto& changed_path : changed_paths) {
        auto old_bytes = co_await impl_->read_at(changed_path, report.base.change);
        if (!old_bytes && old_bytes.error().code != errc::not_found) {
            co_return std::unexpected(old_bytes.error());
        }
        auto new_bytes = std::vector<std::byte>{};
        auto parts = split_path(changed_path);
        if (!parts) {
            co_return std::unexpected(parts.error());
        }
        const auto* current = lookup(impl_->current.root, *parts);
        if (current && current->kind == entry_kind::file) {
            auto loaded = co_await impl_->blob_contents(current->object, changed_path);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            new_bytes = std::move(*loaded);
        }
        const auto before =
            old_bytes ? std::span<const std::byte>{*old_bytes} : std::span<const std::byte>{};
        auto hunks = repository_detail::text_hunks(before, new_bytes);
        report.changed.push_back({changed_path, hunks.value_or(std::vector<line_change>{})});
    }
    co_return report;
}

runtime::task<std::optional<stale_revalidation>>
repository::validate_write(write_options options, std::vector<std::string> paths) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (impl_->base_required_for_writer() && !options.base) {
        co_return std::unexpected(failure(errc::invalid_argument,
                                          "agent writes must name the version they reasoned from"));
    }
    paths.insert(paths.end(), options.depends_on.begin(), options.depends_on.end());
    for (const auto& path : paths) {
        auto parsed = split_path(path);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
    }
    if (!options.base) {
        co_return std::optional<stale_revalidation>{};
    }
    const auto cursor = impl_->base_cursor(*options.base);
    if (!cursor) {
        co_return std::unexpected(cursor.error());
    }
    auto base_snapshot = co_await at(options.base->change);
    if (!base_snapshot) {
        co_return std::unexpected(base_snapshot.error());
    }
    const auto head_moved = !impl_->current.head ||
                            !same_digest(impl_->current.head->value, options.base->change.value);
    const auto journal_moved = *cursor != impl_->current.drips.size();
    if (!head_moved && !journal_moved) {
        co_return std::optional<stale_revalidation>{};
    }
    if (options.on_stale == write_options::stale_action::refuse) {
        co_return std::unexpected(
            failure(errc::conflict, "write base has moved", to_string(options.base->change.value)));
    }
    auto report = co_await explain_stale(*options.base, std::move(paths));
    if (!report) {
        co_return std::unexpected(report.error());
    }
    if (options.on_stale == write_options::stale_action::accept_if_disjoint) {
        if (!report->changed.empty()) {
            co_return std::unexpected(failure(errc::conflict,
                                              "write base moved on a required path",
                                              report->changed.front().path));
        }
        co_return std::optional<stale_revalidation>{stale_revalidation::disjoint};
    }
    co_return std::optional<stale_revalidation>{stale_revalidation::forced};
}

runtime::task<std::vector<drip>> repository::drips() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto output = std::vector<drip>{};
    output.reserve(impl_->current.drips.size());
    for (const auto& saved : impl_->current.drips) {
        auto decoded = decode_repository_drip(saved.body);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        auto id = hash(impl_->hash_algorithm, saved.body);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        auto value = drip{.id = {*id},
                          .base = decoded->base.value_or(change_id{{impl_->hash_algorithm, {}}}),
                          .by = decoded->agent.value_or(saved.author),
                          .signature = saved.signature,
                          .at = std::chrono::sys_seconds{std::chrono::seconds{decoded->at}},
                          .reasoned_from = decoded->reasoned_from,
                          .revalidated = decoded->revalidated,
                          .delegation = decoded->delegation};
        value.identity_verified =
            drip_signed_by_verified_identity(saved, impl_->signing_key.identity());
        for (auto& path : decoded->paths) {
            value.ops.push_back({std::move(path)});
        }
        output.push_back(std::move(value));
    }
    co_return output;
}
runtime::task<std::vector<std::byte>> repository::read(std::string path) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    if (impl_->current.root_is_paged) {
        auto entry = co_await impl_->paged_entry_at(*parts);
        if (!entry) {
            co_return std::unexpected(entry.error());
        }
        if (*entry && (*entry)->kind == node_kind::blob) {
            co_return co_await impl_->blob_contents((*entry)->id, std::move(path));
        }
        if (!*entry || (*entry)->kind != node_kind::conflict) {
            co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
        }
        const auto conflict =
            std::ranges::find(impl_->current.conflict_values, path, &conflict_reference::path);
        if (conflict == impl_->current.conflict_values.end() ||
            conflict->selected_kind != node_kind::blob) {
            co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
        }
        co_return co_await impl_->blob_contents(conflict->selected, std::move(path));
    }
    auto* entry = lookup(impl_->current.root, *parts);
    if (entry && entry->kind == entry_kind::file) {
        co_return co_await impl_->blob_contents(entry->object, std::move(path));
    }
    const auto conflict =
        std::ranges::find(impl_->current.conflict_values, path, &conflict_reference::path);
    if (conflict == impl_->current.conflict_values.end() ||
        conflict->selected_kind != node_kind::blob) {
        co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
    }
    co_return co_await impl_->blob_contents(conflict->selected, std::move(path));
}
runtime::task<std::vector<std::byte>> repository::read_decrypted(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto raw = co_await read(path);
    if (!raw) {
        co_return std::unexpected(raw.error());
    }
    const auto* rule = encryption_rule_for(impl_->current, path);
    if (!rule) {
        co_return raw;
    }
    if (rule->mode == encryption_mode::file) {
        co_return co_await encryption_detail::leaf_cipher::decrypt_file(
            std::move(*raw), std::move(path), rule->key_id, impl_->keys);
    }
    const auto format = leaf_format(path);
    const auto source = std::string_view{reinterpret_cast<const char*>(raw->data()), raw->size()};
    const auto leaf_rule = encryption_detail::leaf_rule{rule->key_id, rule->encrypted_regex};
    if (format == "json") {
        auto parsed = formats::json::parse(source);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto decrypted = co_await encryption_detail::leaf_cipher::decrypt_json(
            std::move(*parsed), leaf_rule, impl_->keys);
        if (!decrypted) {
            co_return std::unexpected(decrypted.error());
        }
        auto written = formats::json::write(*decrypted);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        co_return string_bytes(*written);
    }
    if (format == "dotenv") {
        auto parsed = formats::dotenv::parse(source);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto decrypted = co_await encryption_detail::leaf_cipher::decrypt_dotenv(
            std::move(*parsed), leaf_rule, impl_->keys);
        if (!decrypted) {
            co_return std::unexpected(decrypted.error());
        }
        auto written = formats::dotenv::write(*decrypted);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        co_return string_bytes(*written);
    }
    co_return std::unexpected(
        failure(errc::unsupported, "leaf encryption supports JSON and .env", path));
}
runtime::task<void> repository::write(std::string path, std::vector<std::byte> contents) {
    co_return co_await write(std::move(path), std::move(contents), {});
}
runtime::task<void>
repository::write(std::string path, std::vector<std::byte> contents, write_options options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if ((options.mode & 0170000U) != 0100000U && options.mode != 0120000U)
        co_return std::unexpected(failure(errc::invalid_argument, "invalid file mode", path));
    auto lease = impl_->validate_lease_write(path, options);
    if (!lease) {
        co_return std::unexpected(lease.error());
    }
    auto revalidated = co_await validate_write(options, {path});
    if (!revalidated) {
        co_return std::unexpected(revalidated.error());
    }
    auto lock_allowed = impl_->validate_lock_write(path);
    if (!lock_allowed) {
        co_return std::unexpected(lock_allowed.error());
    }
    const auto update_bytes = contents.size();
    if (update_bytes > impl_->current.flow.max_update_bytes) {
        co_return std::unexpected(
            failure(errc::invalid_argument,
                    "write update exceeds max_update_bytes",
                    "limit=" + std::to_string(impl_->current.flow.max_update_bytes)));
    }
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto* current_entry = lookup(impl_->current.root, *parts);
    const auto* encryption_rule = encryption_rule_for(impl_->current, path);
    auto prior_reference = std::optional<digest>{};
    if (current_entry && current_entry->kind == entry_kind::file) {
        prior_reference = current_entry->object;
    }
    auto prepared = co_await impl_->prepare_write(path, std::move(contents), prior_reference);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    contents = std::move(*prepared);
    auto format_entries = std::vector<digest>{};
    if (!encryption_rule) {
        auto stored_entries = co_await store_format_entries(impl_->objects, contents, path);
        if (!stored_entries) {
            co_return std::unexpected(stored_entries.error());
        }
        format_entries = std::move(*stored_entries);
    }
    auto content = encryption_rule && encryption_rule->mode == encryption_mode::file
                       ? co_await repository_detail::store_access::put_encrypted(
                             impl_->objects, std::move(contents), encryption_rule->key_id)
                   : encryption_rule ? co_await repository_detail::store_access::put_plain(
                                           impl_->objects, std::move(contents))
                                     : co_await impl_->objects.put(std::move(contents));
    if (!content)
        co_return std::unexpected(content.error());
    auto record = impl_->objects.blob_for(*content);
    if (!record)
        co_return std::unexpected(record.error());
    record->format_entries = std::move(format_entries);
    co_return co_await impl_->record_blob(std::move(path),
                                          std::move(*record),
                                          std::move(options),
                                          *revalidated,
                                          std::nullopt,
                                          update_bytes);
}
runtime::task<void>
repository::write_file(std::string path, std::filesystem::path source, write_options options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if ((options.mode & 0170000U) != 0100000U && options.mode != 0120000U) {
        co_return std::unexpected(failure(errc::invalid_argument, "invalid file mode", path));
    }
    auto lease = impl_->validate_lease_write(path, options);
    if (!lease) {
        co_return std::unexpected(lease.error());
    }
    auto revalidated = co_await validate_write(options, {path});
    if (!revalidated) {
        co_return std::unexpected(revalidated.error());
    }
    if (encryption_rule_for(impl_->current, path)) {
        auto contents = co_await store_detail::read_file(source);
        if (!contents)
            co_return std::unexpected(contents.error());
        co_return co_await write(std::move(path), std::move(*contents), std::move(options));
    }
    auto candidate =
        co_await runtime::offload([path, source] { return format_file_candidate(path, source); });
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    if (*candidate) {
        auto contents = co_await store_detail::read_file(source);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        co_return co_await write(std::move(path), std::move(*contents), std::move(options));
    }
    auto content = co_await impl_->objects.put_file(std::move(source));
    if (!content)
        co_return std::unexpected(content.error());
    auto record = impl_->objects.blob_for(*content);
    if (!record)
        co_return std::unexpected(record.error());
    co_return co_await impl_->record_blob(
        std::move(path), std::move(*record), std::move(options), *revalidated);
}

runtime::task<tree_import_report> repository::import_tree(std::filesystem::path root,
                                                          import_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto code = std::error_code{};
    if (!std::filesystem::is_directory(root, code)) {
        co_return std::unexpected(failure(code ? errc::io : errc::not_found,
                                          code ? code.message() : "import root is not a directory",
                                          root.string()));
    }
    auto caller_patterns = std::move(options.ignore_patterns);
    for (const auto name : {".gitignore", ".liquidignore"}) {
        auto ignore_file = std::ifstream{root / name};
        auto pattern = std::string{};
        while (std::getline(ignore_file, pattern)) {
            if (pattern.empty() || pattern.starts_with('#')) {
                continue;
            }
            const auto offset = pattern.starts_with('!') ? std::size_t{1} : std::size_t{};
            if (pattern.size() > offset && pattern[offset] == '/') {
                pattern.erase(offset, 1);
            }
            if (pattern.ends_with('/')) {
                pattern += "**";
            }
            options.ignore_patterns.push_back(std::move(pattern));
        }
    }
    options.ignore_patterns.insert(options.ignore_patterns.end(),
                                   std::make_move_iterator(caller_patterns.begin()),
                                   std::make_move_iterator(caller_patterns.end()));
    auto report = tree_import_report{};
    auto changed_paths = std::vector<std::string>{};
    auto tree_changed = false;
    if (impl_->current.root_is_paged) {
        auto imported = co_await impl_->import_paged_directory(impl_->current.root.object,
                                                               impl_->current.root.node,
                                                               impl_->current.root.mode,
                                                               root,
                                                               {},
                                                               options,
                                                               report,
                                                               changed_paths);
        if (!imported) {
            co_return std::unexpected(imported.error());
        }
        impl_->current.root.object = imported->root;
        impl_->current.root.entries.clear();
        tree_changed = imported->changed;
    } else {
        auto imported = co_await impl_->import_directory(
            impl_->current.root, root, {}, options, report, changed_paths);
        if (!imported) {
            co_return std::unexpected(imported.error());
        }
        tree_changed = report.changed != 0;
    }
    if (!tree_changed) {
        co_return report;
    }
    if (report.changed == 0) {
        auto saved = co_await impl_->checkpoint();
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        co_return report;
    }
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation) {
        co_return std::unexpected(operation.error());
    }
    impl_->record_operation(*operation,
                            "import",
                            root.lexically_normal().generic_string(),
                            std::move(options.message),
                            impl_->current.root.node,
                            std::nullopt);
    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto drip = co_await impl_->append_drip(
        "import", impl_->current.root.node, at, std::move(changed_paths), {}, std::nullopt);
    if (!drip) {
        co_return std::unexpected(drip.error());
    }
    ++impl_->current.pending_drips;
    impl_->directory_cache.clear();
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    report.drip = *operation;
    co_return report;
}

runtime::task<digest> repository::store_remote_blob(::liquid::blob value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto registered =
        co_await repository_detail::store_access::register_remote(impl_->objects, value);
    if (!registered) {
        co_return std::unexpected(registered.error());
    }
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::store_access::put_plain(impl_->objects,
                                                                  std::move(*encoded));
}

runtime::task<void>
repository::write_remote(std::string path, ::liquid::blob value, write_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if ((options.mode & 0170000U) != 0100000U && options.mode != 0120000U) {
        co_return std::unexpected(failure(errc::invalid_argument, "invalid file mode", path));
    }
    auto lease = impl_->validate_lease_write(path, options);
    if (!lease) {
        co_return std::unexpected(lease.error());
    }
    auto revalidated = co_await validate_write(options, {path});
    if (!revalidated) {
        co_return std::unexpected(revalidated.error());
    }
    auto registered =
        co_await repository_detail::store_access::register_remote(impl_->objects, value);
    if (!registered) {
        co_return std::unexpected(registered.error());
    }
    co_return co_await impl_->record_blob(
        std::move(path), std::move(value), std::move(options), *revalidated);
}
runtime::task<void> repository::redact(std::string path, std::optional<change_id> change) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    auto* current = lookup(impl_->current.root, *parts);
    if (!current || current->kind != entry_kind::file) {
        co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
    }
    auto target = std::optional<digest>{};
    if (change) {
        auto historical = co_await impl_->object_at(*change, *parts);
        if (!historical) {
            co_return std::unexpected(historical.error());
        }
        target = *historical;
    } else {
        for (auto operation = impl_->current.operations.rbegin();
             operation != impl_->current.operations.rend();
             ++operation) {
            if (operation->kind == "write" && operation->from == path && operation->prior &&
                to_string(operation->prior->object) != to_string(current->object)) {
                target = operation->prior->object;
                break;
            }
        }
    }
    if (!target) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "redacted content or a change id is required", path));
    }
    auto replacement = co_await read(path);
    if (!replacement) {
        co_return std::unexpected(replacement.error());
    }
    co_return co_await redact_impl(std::move(path), std::move(*replacement), *target);
}

runtime::task<void> repository::redact(std::string path,
                                       std::vector<std::byte> replacement,
                                       std::optional<change_id> change) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    auto* current = lookup(impl_->current.root, *parts);
    if (!current || current->kind != entry_kind::file) {
        co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
    }
    auto target = current->object;
    if (change) {
        auto historical = co_await impl_->object_at(*change, *parts);
        if (!historical) {
            co_return std::unexpected(historical.error());
        }
        target = *historical;
    } else {
        auto current_contents = co_await read(path);
        if (!current_contents) {
            co_return std::unexpected(current_contents.error());
        }
        if (*current_contents == replacement) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "redacted content is unchanged", path));
        }
    }
    co_return co_await redact_impl(std::move(path), std::move(replacement), target);
}

runtime::task<void>
repository::redact_impl(std::string path, std::vector<std::byte> replacement, digest target) {
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    auto* original = lookup(impl_->current.root, *parts);
    if (!original || original->kind != entry_kind::file) {
        co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
    }
    auto target_references = std::size_t{};
    auto entries = std::vector<const working_entry*>{&impl_->current.root};
    while (!entries.empty()) {
        const auto* entry = entries.back();
        entries.pop_back();
        if (entry->kind == entry_kind::file && to_string(entry->object) == to_string(target)) {
            ++target_references;
        }
        for (const auto& item : entry->entries) {
            entries.push_back(&item.second);
        }
    }
    const auto replaced_reference = to_string(original->object) == to_string(target) ? 1U : 0U;
    if (target_references > replaced_reference) {
        co_return std::unexpected(
            failure(errc::busy, "redaction target remains at another working path", path));
    }
    auto encoded_target = co_await impl_->objects.get(target);
    if (!encoded_target) {
        co_return std::unexpected(encoded_target.error());
    }
    auto old_blob = objects::decode_blob(*encoded_target);
    if (!old_blob) {
        co_return std::unexpected(old_blob.error());
    }
    auto written = co_await write(path, std::move(replacement));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto* current = lookup(impl_->current.root, *parts);
    if (!current || current->kind != entry_kind::file) {
        co_return std::unexpected(failure(errc::internal, "redacted file disappeared", path));
    }
    if (to_string(current->object) == to_string(target)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "redacted content preserves the old digest", path));
    }
    auto encoded_replacement = co_await impl_->objects.get(current->object);
    if (!encoded_replacement) {
        co_return std::unexpected(encoded_replacement.error());
    }
    auto new_blob = objects::decode_blob(*encoded_replacement);
    if (!new_blob) {
        co_return std::unexpected(new_blob.error());
    }
    auto live_content = std::set<std::string>{};
    auto pending_entries = std::vector<const working_entry*>{&impl_->current.root};
    while (!pending_entries.empty()) {
        const auto* entry = pending_entries.back();
        pending_entries.pop_back();
        if (entry->kind == entry_kind::file) {
            auto encoded = co_await impl_->objects.get(entry->object);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto value = objects::decode_blob(*encoded);
            if (!value) {
                co_return std::unexpected(value.error());
            }
            for (const auto& chunk : value->chunks) {
                live_content.insert(to_string(chunk));
            }
            continue;
        }
        for (const auto& [name, child] : entry->entries) {
            pending_entries.push_back(&child);
        }
    }
    auto purge = std::vector<digest>{target};
    for (const auto& chunk : old_blob->chunks) {
        if (!live_content.contains(to_string(chunk))) {
            purge.push_back(chunk);
        }
    }
    auto operation_hash = hash(impl_->hash_algorithm, std::span<const std::byte>{target.bytes});
    if (!operation_hash) {
        co_return std::unexpected(operation_hash.error());
    }
    auto prior = working_entry{entry_kind::file, current->node, current->mode, target, {}};
    auto redaction_operation = stored_operation{};
    redaction_operation.id = op_id{*operation_hash};
    redaction_operation.kind = "redact";
    redaction_operation.from = path;
    redaction_operation.node = current->node;
    redaction_operation.prior = prior;
    redaction_operation.entries.push_back({path, prior, *current});
    impl_->record_operation(std::move(redaction_operation));
    auto recorded = co_await impl_->append_redaction(path, current->node, target);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    auto removed = co_await impl_->objects.redact(std::move(purge));
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    auto verification = co_await impl_->objects.get(target);
    if (verification || verification.error().code != errc::not_found) {
        co_return std::unexpected(failure(errc::internal, "redaction verification failed", path));
    }
    co_return result<void>{};
}

runtime::task<std::vector<std::string>>
repository::diff_keys(std::string path, change_id change_a, change_id change_b) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto* rule = encryption_rule_for(impl_->current, path);
    if (!rule) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "path is not declared encrypted", path));
    }
    auto left = co_await impl_->read_at(path, change_a);
    if (!left) {
        co_return std::unexpected(left.error());
    }
    auto right = co_await impl_->read_at(path, change_b);
    if (!right) {
        co_return std::unexpected(right.error());
    }
    if (rule->mode == encryption_mode::file) {
        if (*left == *right) {
            co_return std::vector<std::string>{};
        }
        co_return std::vector<std::string>{path};
    }
    const auto format = leaf_format(path);
    const auto left_source =
        std::string_view{reinterpret_cast<const char*>(left->data()), left->size()};
    const auto right_source =
        std::string_view{reinterpret_cast<const char*>(right->data()), right->size()};
    const auto leaf_rule = encryption_detail::leaf_rule{rule->key_id, rule->encrypted_regex};
    if (format == "json") {
        auto left_document = formats::json::parse(left_source);
        auto right_document = formats::json::parse(right_source);
        if (!left_document) {
            co_return std::unexpected(left_document.error());
        }
        if (!right_document) {
            co_return std::unexpected(right_document.error());
        }
        auto left_valid = encryption_detail::validate_json(*left_document, leaf_rule);
        auto right_valid = encryption_detail::validate_json(*right_document, leaf_rule);
        if (!left_valid) {
            co_return std::unexpected(left_valid.error());
        }
        if (!right_valid) {
            co_return std::unexpected(right_valid.error());
        }
        co_return encryption_detail::diff_json(*left_document, *right_document);
    }
    if (format == "dotenv") {
        auto left_document = formats::dotenv::parse(left_source);
        auto right_document = formats::dotenv::parse(right_source);
        if (!left_document) {
            co_return std::unexpected(left_document.error());
        }
        if (!right_document) {
            co_return std::unexpected(right_document.error());
        }
        auto left_valid = encryption_detail::validate_dotenv(*left_document, leaf_rule);
        auto right_valid = encryption_detail::validate_dotenv(*right_document, leaf_rule);
        if (!left_valid) {
            co_return std::unexpected(left_valid.error());
        }
        if (!right_valid) {
            co_return std::unexpected(right_valid.error());
        }
        co_return encryption_detail::diff_dotenv(*left_document, *right_document);
    }
    co_return std::unexpected(
        failure(errc::unsupported, "leaf encryption supports JSON and .env", path));
}
runtime::task<void> repository::move(std::string from, std::string to, write_options options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto revalidated = co_await validate_write(options, {from, to});
    if (!revalidated) {
        co_return std::unexpected(revalidated.error());
    }
    auto source_lock = impl_->validate_lock_write(from);
    auto destination_lock = impl_->validate_lock_write(to);
    if (!source_lock || !destination_lock) {
        co_return std::unexpected(source_lock ? destination_lock.error() : source_lock.error());
    }
    auto source_lease = impl_->validate_lease_write(from, options);
    auto destination_lease = impl_->validate_lease_write(to, options);
    if (!source_lease || !destination_lease) {
        co_return std::unexpected(!source_lease ? source_lease.error() : destination_lease.error());
    }
    auto source = split_path(from);
    auto destination = split_path(to);
    if (!source)
        co_return std::unexpected(source.error());
    if (!destination)
        co_return std::unexpected(destination.error());
    if (to.starts_with(from + "/"))
        co_return std::unexpected(
            failure(errc::invalid_argument, "move destination is below source", to));
    auto source_parent = parent_for(impl_->current.root, *source, false, impl_->hash_algorithm);
    auto destination_parent =
        parent_for(impl_->current.root, *destination, false, impl_->hash_algorithm);
    if (!source_parent || !destination_parent)
        co_return std::unexpected(failure(errc::not_found, "move parent does not exist", from));
    auto found = (*source_parent)->entries.find(source->back());
    if (found == (*source_parent)->entries.end())
        co_return std::unexpected(failure(errc::not_found, "move source does not exist", from));
    if ((*destination_parent)->entries.contains(destination->back()))
        co_return std::unexpected(failure(errc::already_exists, "move destination exists", to));
    auto admitted = impl_->flow_limits->admit(impl_->current_actor().taap_id,
                                              flow_detail::node_key((*source_parent)->node),
                                              from.size() + to.size() + 32);
    if (!admitted)
        co_return std::unexpected(admitted.error());
    auto source_document = co_await impl_->directory((*source_parent)->node);
    if (!source_document)
        co_return std::unexpected(source_document.error());
    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto time =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    auto commit_options = crdt::commit_options{.message = "move", .time = time};
    if ((*source_parent)->node == (*destination_parent)->node) {
        auto recorded =
            (*source_document)->rename_entry(source->back(), destination->back(), commit_options);
        if (!recorded)
            co_return std::unexpected(recorded.error());
    } else {
        auto destination_document = co_await impl_->directory((*destination_parent)->node);
        if (!destination_document)
            co_return std::unexpected(destination_document.error());
        auto removed = (*source_document)->remove_entry(source->back(), commit_options);
        if (!removed)
            co_return std::unexpected(removed.error());
        const auto kind = found->second.kind == entry_kind::file        ? "blob"
                          : found->second.kind == entry_kind::directory ? "tree"
                                                                        : "repo";
        auto inserted =
            (*destination_document)
                ->put_entry(
                    destination->back(),
                    {kind, found->second.node, found->second.mode, to_string(found->second.object)},
                    commit_options);
        if (!inserted)
            co_return std::unexpected(inserted.error());
    }
    auto moved = std::move(found->second);
    const auto moved_node = moved.node;
    (*source_parent)->entries.erase(found);
    (*destination_parent)->entries.emplace(destination->back(), std::move(moved));
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation)
        co_return std::unexpected(operation.error());
    impl_->record_operation(*operation, "move", from, to, moved_node, std::nullopt);
    auto signed_drip =
        co_await impl_->append_drip("move", moved_node, at, {from, to}, options, *revalidated);
    if (!signed_drip)
        co_return std::unexpected(signed_drip.error());
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<void> repository::remove(std::string path, write_options options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto revalidated = co_await validate_write(options, {path});
    if (!revalidated) {
        co_return std::unexpected(revalidated.error());
    }
    auto lock_allowed = impl_->validate_lock_write(path);
    if (!lock_allowed) {
        co_return std::unexpected(lock_allowed.error());
    }
    auto lease = impl_->validate_lease_write(path, options);
    if (!lease) {
        co_return std::unexpected(lease.error());
    }
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, false, impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    auto found = (*parent)->entries.find(parts->back());
    if (found == (*parent)->entries.end())
        co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
    auto admitted = impl_->flow_limits->admit(
        impl_->current_actor().taap_id, flow_detail::node_key((*parent)->node), path.size() + 32);
    if (!admitted)
        co_return std::unexpected(admitted.error());
    auto directory = co_await impl_->directory((*parent)->node);
    if (!directory)
        co_return std::unexpected(directory.error());
    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto time =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    auto recorded =
        (*directory)
            ->remove_entry(parts->back(), crdt::commit_options{.message = "remove", .time = time});
    if (!recorded)
        co_return std::unexpected(recorded.error());
    auto prior = found->second;
    (*parent)->entries.erase(found);
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation)
        co_return std::unexpected(operation.error());
    impl_->record_operation(*operation, "remove", path, {}, prior.node, std::move(prior));
    auto signed_drip =
        co_await impl_->append_drip("remove", prior.node, at, {path}, options, *revalidated);
    if (!signed_drip)
        co_return std::unexpected(signed_drip.error());
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<change_id> repository::cut(std::string message) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    change_input input{};
    if (impl_->current.head)
        input.parents.push_back({impl_->current.repository, *impl_->current.head});
    input.author = impl_->signing_key.identity();
    input.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    input.message = std::move(message);
    input.ref = "main";
    co_return co_await record(std::move(input));
}
runtime::task<change_id> repository::record(change_input input) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto root = result<digest>{impl_->current.root.object};
    if (!impl_->current.root_is_paged || !impl_->current.root.entries.empty()) {
        root = co_await impl_->snapshot(impl_->current.root);
        if (!root)
            co_return std::unexpected(root.error());
    }
    const auto recorded = co_await record_from_tree(std::move(input), *root);
    if (!recorded)
        co_return std::unexpected(recorded.error());
    auto saved = co_await impl_->checkpoint(*root);
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return *recorded;
}
runtime::task<change_id> repository::record_from_tree(change_input input, digest root) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    for (auto& parent : input.parents)
        if (std::ranges::all_of(parent.repo_id,
                                [](std::byte value) { return value == std::byte{}; }))
            parent.repo_id = impl_->current.repository;
    change value{};
    value.parents = std::move(input.parents);
    value.author = std::move(input.author);
    value.message = std::move(input.message);
    value.at = input.at;
    value.root = root;
    value.metadata = std::move(input.metadata);
    value.conflicts = impl_->current.conflict_values;
    value.resolutions = impl_->current.pending_resolutions;
    value.document = public_document_state(impl_->current);
    auto document_heads = co_await impl_->capture_document_heads();
    if (!document_heads) {
        co_return std::unexpected(document_heads.error());
    }
    value.heads = std::move(*document_heads);
    value.record_version = 3;
    auto body = objects::encode_change_body(value);
    if (!body)
        co_return std::unexpected(body.error());
    auto signed_value = co_await impl_->signing_key.sign("change", std::move(*body));
    if (!signed_value)
        co_return std::unexpected(signed_value.error());
    auto signature = identity_detail::canonical_signature(*signed_value);
    if (!signature)
        co_return std::unexpected(signature.error());
    value.sig = std::move(*signature);
    auto encoded = objects::encode(value);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    auto stored =
        co_await repository_detail::store_access::put_plain(impl_->objects, std::move(*encoded));
    if (!stored)
        co_return std::unexpected(stored.error());
    impl_->current.pending_resolutions.clear();
    change_id id{*stored};
    if (input.ref) {
        const auto previous_head = impl_->current.head;
        auto previous_ref = std::optional<change_id>{};
        if (const auto found = impl_->current.refs.find(*input.ref);
            found != impl_->current.refs.end()) {
            previous_ref = found->second;
        }
        impl_->current.head = id;
        impl_->current.refs[*input.ref] = id;
        auto operation_id = random_operation_id(impl_->hash_algorithm);
        if (!operation_id) {
            co_return std::unexpected(operation_id.error());
        }
        auto operation = stored_operation{};
        operation.id = *operation_id;
        operation.kind = "record";
        operation.from = *input.ref;
        operation.refs.push_back({*input.ref, previous_ref, id});
        operation.head_before = previous_head;
        operation.head_after = id;
        impl_->record_operation(std::move(operation));
    }
    co_return id;
}

runtime::task<digest> repository_detail::git_access::put_blob(repository& destination,
                                                              std::string delta_scope,
                                                              std::vector<std::byte> contents) {
    if (!destination.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto content = co_await repository_detail::store_access::put_plain(
        destination.impl_->objects, std::move(contents), std::move(delta_scope));
    if (!content)
        co_return std::unexpected(content.error());
    auto value = repository_detail::store_access::blob_for(destination.impl_->objects, *content);
    if (!value)
        co_return std::unexpected(value.error());
    value->content = {};
    auto encoded = objects::encode(*value);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    co_return co_await repository_detail::store_access::put_plain(destination.impl_->objects,
                                                                  std::move(*encoded));
}

repository_detail::git_batch::~git_batch() {
    if (active_ && destination_)
        repository_detail::git_access::abort_batch(*destination_);
}

repository_detail::git_batch::git_batch(git_batch&& source) noexcept
    : destination_(std::exchange(source.destination_, nullptr)),
      active_(std::exchange(source.active_, false)) {}

auto repository_detail::git_batch::operator=(git_batch&& source) noexcept -> git_batch& {
    if (this == &source)
        return *this;
    if (active_ && destination_)
        repository_detail::git_access::abort_batch(*destination_);
    destination_ = std::exchange(source.destination_, nullptr);
    active_ = std::exchange(source.active_, false);
    return *this;
}

runtime::task<void> repository_detail::git_batch::commit() {
    if (!active_ || !destination_) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "Git object batch is not active"));
    }
    const auto committed = co_await repository_detail::git_access::commit_batch(*destination_);
    if (!committed) {
        co_return std::unexpected(committed.error());
    }
    active_ = false;
    co_return result<void>{};
}

repository_detail::git_batch repository_detail::git_access::batch(repository& destination) {
    repository_detail::store_access::begin_object_batch(destination.impl_->objects);
    auto output = git_batch{};
    output.destination_ = &destination;
    output.active_ = true;
    return output;
}

void repository_detail::git_access::abort_batch(repository& destination) {
    if (destination.impl_)
        repository_detail::store_access::abort_object_batch(destination.impl_->objects);
}

runtime::task<void> repository_detail::git_access::commit_batch(repository& destination) {
    if (!destination.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await repository_detail::store_access::commit_object_batch(
        destination.impl_->objects);
}

runtime::task<digest> repository_detail::git_access::put_tree(repository& destination, tree value) {
    if (!destination.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto encoded = objects::encode(value);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    co_return co_await repository_detail::store_access::put_plain(destination.impl_->objects,
                                                                  std::move(*encoded));
}
runtime::task<std::optional<journal_cursor>> repository::replication_cursor(std::string source) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto valid = valid_replication_source(source);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto replication = impl_->state;
    replication.prefix = replication.replication_prefix;
    auto stored = co_await read_state(replication, source + ".cursor");
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::optional<journal_cursor>{};
    }
    co_return std::optional<journal_cursor>{journal_cursor{bytes_text(**stored)}};
}
runtime::task<void> repository::set_replication_cursor(std::string source, journal_cursor cursor) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto valid = valid_replication_source(source);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto replication = impl_->state;
    replication.prefix = replication.replication_prefix;
    co_return co_await publish_state(replication, source + ".cursor", string_bytes(cursor.token));
}
runtime::task<void> repository::set_metadata(std::string key, std::vector<std::byte> value) {
    if (!impl_ || key.empty())
        co_return std::unexpected(failure(errc::invalid_argument, "invalid metadata key", key));
    impl_->current.metadata[std::move(key)] = std::move(value);
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<std::optional<std::vector<std::byte>>> repository::metadata(std::string key) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    const auto found = impl_->current.metadata.find(key);
    if (found == impl_->current.metadata.end())
        co_return std::optional<std::vector<std::byte>>{};
    co_return std::optional<std::vector<std::byte>>{found->second};
}
runtime::task<std::vector<std::string>> repository::metadata_keys(std::string prefix) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    std::vector<std::string> keys;
    for (const auto& [key, value] : impl_->current.metadata) {
        static_cast<void>(value);
        if (key.starts_with(prefix))
            keys.push_back(key);
    }
    co_return keys;
}
runtime::task<std::vector<ref>> repository::refs() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    std::vector<ref> output;
    for (const auto& [name, id] : impl_->current.refs)
        output.push_back({name, id});
    co_return output;
}
runtime::task<void> repository::set_ref(std::string name, change_id id) {
    if (!impl_ || name.empty())
        co_return std::unexpected(
            failure(errc::invalid_argument, "invalid repository reference", name));
    auto previous = std::optional<change_id>{};
    if (const auto found = impl_->current.refs.find(name); found != impl_->current.refs.end()) {
        previous = found->second;
    }
    impl_->current.refs[name] = id;
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "set_ref";
    operation.from = name;
    operation.refs.push_back({name, previous, id});
    impl_->record_operation(std::move(operation));
    auto saved = co_await impl_->checkpoint();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
result<repository_status> repository::status() {
    if (!impl_)
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    repository_status output{};
    output.pending_drips = impl_->current.pending_drips;
    output.conflicts = impl_->current.conflict_values;
    output.head = impl_->current.head;
    output.agents = impl_->current.agents;
    for (const auto& branch : impl_->current.data_branches) {
        output.data_branches.push_back(branch.record);
    }
    output.store_recoveries = repository_detail::store_access::recorded_recoveries(impl_->objects);
    auto visit = [&](auto&& self, const working_entry& entry, std::string path) -> result<void> {
        if (entry.kind == entry_kind::file) {
            auto remote =
                repository_detail::store_access::remote_bytes(impl_->objects, entry.object);
            if (!remote) {
                return std::unexpected(remote.error());
            }
            output.remote_bytes += *remote;
        }
        if (entry.kind == entry_kind::directory && entry.entries.size() > 10000) {
            output.oversized_directories.push_back(path);
        }
        for (const auto& [name, child] : entry.entries) {
            auto visited = self(self, child, path.empty() ? name : path + "/" + name);
            if (!visited) {
                return std::unexpected(visited.error());
            }
        }
        return {};
    };
    auto visited = visit(visit, impl_->current.root, "");
    if (!visited) {
        return std::unexpected(visited.error());
    }
    return output;
}
runtime::task<std::vector<change>> repository::log(log_options options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto next = options.from ? options.from : impl_->current.head;
    if (options.first_parent) {
        std::vector<change> output;
        while (next) {
            auto bytes = co_await impl_->objects.get(next->value);
            if (!bytes)
                co_return std::unexpected(bytes.error());
            auto value = objects::decode_change(*bytes, next->value.alg);
            if (!value)
                co_return std::unexpected(value.error());
            value->identity_verified = change_signed_by_verified_identity(*value);
            output.push_back(*value);
            if (value->parents.empty() ||
                !history_repository(impl_->current, value->parents.front().repo_id)) {
                next.reset();
            } else {
                next = value->parents.front().id;
            }
        }
        co_return output;
    }

    std::vector<change> changes;
    std::map<std::string, std::size_t> indexes;
    std::vector<change_id> pending;
    if (next)
        pending.push_back(*next);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto key = to_string(id.value);
        if (indexes.contains(key))
            continue;
        auto bytes = co_await impl_->objects.get(id.value);
        if (!bytes)
            co_return std::unexpected(bytes.error());
        auto value = objects::decode_change(*bytes, id.value.alg);
        if (!value)
            co_return std::unexpected(value.error());
        value->identity_verified = change_signed_by_verified_identity(*value);
        indexes.emplace(key, changes.size());
        changes.push_back(*value);
        for (const auto& parent : value->parents)
            if (history_repository(impl_->current, parent.repo_id))
                pending.push_back(parent.id);
    }

    std::vector<std::uint64_t> children(changes.size());
    for (const auto& value : changes)
        for (const auto& parent : value.parents)
            if (history_repository(impl_->current, parent.repo_id)) {
                const auto found = indexes.find(to_string(parent.id.value));
                if (found != indexes.end())
                    ++children[found->second];
            }
    auto earlier = [&](std::size_t left, std::size_t right) {
        if (changes[left].at != changes[right].at)
            return changes[left].at > changes[right].at;
        return to_string(changes[left].id.value) < to_string(changes[right].id.value);
    };
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < children.size(); ++index)
        if (children[index] == 0)
            ready.push_back(index);
    std::ranges::sort(ready, earlier);
    std::vector<change> output;
    while (!ready.empty()) {
        const auto index = ready.front();
        ready.erase(ready.begin());
        output.push_back(changes[index]);
        for (const auto& parent : changes[index].parents) {
            if (!history_repository(impl_->current, parent.repo_id))
                continue;
            const auto found = indexes.find(to_string(parent.id.value));
            if (found == indexes.end())
                continue;
            auto& count = children[found->second];
            if (--count == 0) {
                ready.push_back(found->second);
                std::ranges::sort(ready, earlier);
            }
        }
    }
    co_return output;
}
runtime::task<snapshot> repository::at(change_id id) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto bytes = co_await impl_->objects.get(id.value);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    auto change = objects::decode_change(*bytes, impl_->hash_algorithm);
    if (!change)
        co_return std::unexpected(change.error());
    auto tree_bytes = co_await impl_->objects.get(change->root);
    if (!tree_bytes)
        co_return std::unexpected(tree_bytes.error());
    auto tree = objects::decode_tree(*tree_bytes);
    if (!tree)
        co_return std::unexpected(tree.error());
    co_return snapshot{id, *tree};
}
runtime::task<document_state> repository::document_at(change_id id) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto bytes = co_await impl_->objects.get(id.value);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto change = objects::decode_change(*bytes, impl_->hash_algorithm);
    if (!change) {
        co_return std::unexpected(change.error());
    }
    if (change->record_version < 2) {
        co_return std::unexpected(failure(errc::unsupported,
                                          "change does not record historical document state",
                                          to_string(id.value)));
    }
    for (const auto& recorded : change->heads) {
        const auto archive_id = impl_->current.dirs.find(recorded.directory);
        if (archive_id == impl_->current.dirs.end()) {
            co_return std::unexpected(failure(
                errc::not_found, "historical directory archive is absent", to_string(id.value)));
        }
        auto archive_bytes = co_await impl_->objects.get(archive_id->second);
        if (!archive_bytes) {
            co_return std::unexpected(archive_bytes.error());
        }
        auto archive = crdt::directory_document::load_sync(
            *archive_bytes, "historical-read", impl_->root_path.generic_string());
        if (!archive) {
            co_return std::unexpected(archive.error());
        }
        auto valid = archive->validate_heads(recorded.values);
        if (!valid) {
            co_return std::unexpected(valid.error());
        }
    }
    co_return change->document;
}
runtime::task<::liquid::tree> repository::tree(digest id) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto bytes = co_await impl_->objects.get(id);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    co_return objects::decode_tree(*bytes);
}
runtime::task<std::vector<std::byte>> repository::blob(digest id) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto encoded = co_await impl_->objects.get(id);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    auto value = objects::decode_blob(*encoded);
    if (!value)
        co_return std::unexpected(value.error());
    if (value->content.alg == algorithm::blake3 || value->content.alg == algorithm::sha256 ||
        value->content.alg == algorithm::blake3_keyed) {
        co_return co_await impl_->objects.get(value->content);
    }
    std::vector<std::byte> output;
    output.reserve(static_cast<std::size_t>(value->size));
    for (const auto& chunk : value->chunks) {
        auto bytes = co_await impl_->objects.get(chunk);
        if (!bytes)
            co_return std::unexpected(bytes.error());
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    co_return output;
}
runtime::task<std::vector<digest>> repository::record_ids(std::string kind) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return impl_->objects.object_ids(kind);
}
runtime::task<readability_report> repository::readability(std::optional<change_id> revision) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    readability_report output;
    output.limits = {
        "Rotation does not rewrite history.",
        "Revoking a collaborator prevents future access; it does not recall what they have.",
        "Metadata — paths, sizes, change structure and timing — remains visible unless separately "
        "protected."};
    std::vector<change> revisions;
    if (revision) {
        auto bytes = co_await impl_->objects.get(revision->value);
        if (!bytes)
            co_return std::unexpected(bytes.error());
        auto value = objects::decode_change(*bytes, impl_->hash_algorithm);
        if (!value)
            co_return std::unexpected(value.error());
        value->id = *revision;
        revisions.push_back(std::move(*value));
    } else {
        auto next = impl_->current.refs.contains("main")
                        ? std::optional<change_id>{impl_->current.refs.at("main")}
                        : impl_->current.head;
        while (next) {
            auto bytes = co_await impl_->objects.get(next->value);
            if (!bytes)
                co_return std::unexpected(bytes.error());
            auto value = objects::decode_change(*bytes, impl_->hash_algorithm);
            if (!value)
                co_return std::unexpected(value.error());
            value->id = *next;
            next = value->parents.empty() ? std::optional<change_id>{}
                                          : std::optional<change_id>{value->parents.front().id};
            revisions.push_back(std::move(*value));
        }
    }
    for (const auto& revision_value : revisions) {
        std::map<std::pair<std::string, std::uint64_t>, bool> keys;
        const auto record_key = [&](digest id) -> runtime::task<void> {
            auto version = co_await impl_->objects.key_version(id);
            if (!version)
                co_return std::unexpected(version.error());
            if (*version) {
                auto [found, inserted] = keys.emplace(
                    std::pair{(*version)->key_id, (*version)->version}, (*version)->readable);
                if (!inserted)
                    found->second = found->second && (*version)->readable;
            }
            co_return result<void>{};
        };
        std::function<runtime::task<void>(digest)> visit_tree;
        visit_tree = [&](digest tree_id) -> runtime::task<void> {
            auto tree_bytes = co_await impl_->objects.get(tree_id);
            if (!tree_bytes)
                co_return std::unexpected(tree_bytes.error());
            auto tree_value = objects::decode_tree(*tree_bytes);
            if (!tree_value)
                co_return std::unexpected(tree_value.error());
            for (const auto& entry : tree_value->entries) {
                if (entry.kind == node_kind::tree) {
                    auto visited = co_await visit_tree(entry.ref);
                    if (!visited)
                        co_return std::unexpected(visited.error());
                    continue;
                }
                if (entry.kind != node_kind::blob)
                    continue;
                auto recorded = co_await record_key(entry.ref);
                if (!recorded)
                    co_return std::unexpected(recorded.error());
                auto blob_bytes = co_await impl_->objects.get(entry.ref);
                if (!blob_bytes) {
                    if (blob_bytes.error().code == errc::unauthorized)
                        continue;
                    co_return std::unexpected(blob_bytes.error());
                }
                auto blob_value = objects::decode_blob(*blob_bytes);
                if (!blob_value)
                    co_return std::unexpected(blob_value.error());
                for (const auto& chunk : blob_value->chunks) {
                    recorded = co_await record_key(chunk);
                    if (!recorded)
                        co_return std::unexpected(recorded.error());
                }
            }
            co_return result<void>{};
        };
        auto visited = co_await visit_tree(revision_value.root);
        if (!visited)
            co_return std::unexpected(visited.error());
        revision_readability readable;
        readable.revision = revision_value.id;
        for (const auto& [key, held] : keys)
            readable.keys.push_back({key.first, key.second, held});
        output.revisions.push_back(std::move(readable));
    }
    co_return output;
}
runtime::task<void> repository::nest(repository source, std::string path) {
    if (!impl_ || !source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, true, impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    if ((*parent)->entries.contains(parts->back()))
        co_return std::unexpected(failure(errc::already_exists, "nest destination exists", path));
    auto saved_source = co_await source.impl_->checkpoint();
    if (!saved_source) {
        co_return std::unexpected(saved_source.error());
    }
    auto copied = co_await impl_->import_composition(
        *source.impl_, source.impl_->current.root, source.impl_->current.head);
    if (!copied) {
        co_return std::unexpected(copied.error());
    }
    auto nested = source.impl_->current.root;
    nested.kind = entry_kind::nested_repository;
    if (source.impl_->current.head)
        nested.nested = repo_ref{source.impl_->current.repository, *source.impl_->current.head};
    (*parent)->entries.emplace(parts->back(), std::move(nested));
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "nest";
    operation.from = path;
    operation.entries.push_back({path, std::nullopt, (*parent)->entries.at(parts->back())});
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<repository> repository::split(std::string path, std::filesystem::path into) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    auto* entry = lookup(impl_->current.root, *parts);
    if (!entry || entry->kind == entry_kind::file) {
        co_return std::unexpected(failure(errc::not_found, "subtree does not exist", path));
    }
    into = std::filesystem::absolute(into).lexically_normal();
    auto prepared = co_await runtime::offload([into] { return prepare_split_directory(into); });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    auto staging = split_staging_directory{std::move(**prepared)};
    repository_options options;
    options.checkpoint_interval = impl_->checkpoint_interval;
    options.session = impl_->session;
    options.storage.hash_algorithm = impl_->hash_algorithm;
    auto destination = co_await create(staging.path, options);
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    auto initialized = co_await destination->initialize_split(*this, path);
    if (!initialized) {
        co_return std::unexpected(initialized.error());
    }
    // Store handles retain filesystem paths. Close before moving, then reopen at the final path.
    *destination = {};
    auto published = co_await runtime::offload(
        [from = staging.path, into] { return publish_split_directory(from, into); });
    if (!published) {
        co_return std::unexpected(published.error());
    }
    if (!*published) {
        co_return std::unexpected(published->error());
    }
    staging.path = into;
    destination = co_await open(into, options);
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    auto recorded = co_await retain_split(*destination, path);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    staging.path.clear();
    co_return std::move(*destination);
}

runtime::task<void> repository::initialize_split(repository& source, const std::string& path) {
    auto parts = split_path(path);
    auto* entry = lookup(source.impl_->current.root, *parts);
    auto saved = co_await source.impl_->checkpoint();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    const auto nested = entry->kind == entry_kind::nested_repository;
    if (nested && !entry->nested) {
        co_return std::unexpected(failure(errc::not_found, "nested history is unavailable", path));
    }
    const auto head = nested ? std::optional{entry->nested->id} : source.impl_->current.head;
    auto copied = co_await impl_->import_composition(*source.impl_, *entry, head);
    if (!copied) {
        co_return std::unexpected(copied.error());
    }
    impl_->current.root = *entry;
    impl_->current.root.kind = entry_kind::directory;
    impl_->current.root.nested.reset();
    impl_->directory_cache.clear();
    if (nested) {
        impl_->current.repository = entry->nested->repo_id;
        impl_->current.head = head;
        impl_->current.refs["main"] = *head;
        co_return co_await impl_->checkpoint();
    }
    auto input = change_input{};
    if (head) {
        input.parents.push_back({source.impl_->current.repository, *head});
    }
    input.author = impl_->signing_key.identity();
    input.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    input.message = "split " + path;
    input.ref = "main";
    auto recorded = co_await record(std::move(input));
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    co_return result<void>{};
}

runtime::task<void> repository::retain_split(repository& destination, const std::string& path) {
    auto copied = co_await copy_composition_objects(
        destination.impl_->objects, impl_->objects, {destination.impl_->current.head->value});
    if (!copied) {
        co_return std::unexpected(copied.error());
    }
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    const auto before = impl_->current;
    auto parts = split_path(path);
    auto* entry = lookup(impl_->current.root, *parts);
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "split";
    operation.from = path;
    operation.entries.push_back({path, *entry, std::nullopt});
    entry->kind = entry_kind::nested_repository;
    entry->object = destination.impl_->current.root.object;
    entry->nested =
        repo_ref{destination.impl_->current.repository, *destination.impl_->current.head};
    operation.entries.back().after = *entry;
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        impl_->current = before;
        impl_->directory_cache.clear();
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}
runtime::task<void> repository::combine(repository other, std::optional<std::string> at) {
    if (!impl_ || !other.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    const auto path = at.value_or("combined");
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, true, impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    auto before = std::optional<working_entry>{};
    if (const auto found = (*parent)->entries.find(parts->back());
        found != (*parent)->entries.end()) {
        before = found->second;
    }
    if ((*parent)->entries.contains(parts->back())) {
        const auto& existing = (*parent)->entries.at(parts->back());
        if (auto value = repository_detail::entry_conflict(path,
                                                           existing.object,
                                                           other.impl_->current.root.object,
                                                           existing.node,
                                                           other.impl_->current.root.node)) {
            auto stored = co_await impl_->store_conflict_value(std::move(*value));
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
        }
    } else {
        auto copy = other.impl_->current.root;
        copy.kind = entry_kind::nested_repository;
        if (other.impl_->current.head)
            copy.nested = repo_ref{other.impl_->current.repository, *other.impl_->current.head};
        (*parent)->entries.emplace(parts->back(), std::move(copy));
    }
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "combine";
    operation.from = path;
    const auto after = (*parent)->entries.find(parts->back());
    operation.entries.push_back({path,
                                 std::move(before),
                                 after == (*parent)->entries.end() ? std::optional<working_entry>{}
                                                                   : std::optional{after->second}});
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<void> repository::nest_ref(std::string path, repo_ref reference) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, true, impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    if ((*parent)->entries.contains(parts->back()))
        co_return std::unexpected(failure(errc::already_exists, "nest destination exists", path));
    const auto stored = co_await impl_->objects.put(std::vector<std::byte>{});
    if (!stored)
        co_return std::unexpected(stored.error());
    (*parent)->entries.emplace(
        parts->back(),
        working_entry{entry_kind::nested_repository, random_id(), 0160000, *stored, {}, reference});
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "nest";
    operation.from = path;
    operation.entries.push_back({path, std::nullopt, (*parent)->entries.at(parts->back())});
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<std::vector<conflict>> repository::conflicts() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto output = std::vector<conflict>{};
    output.reserve(impl_->current.conflict_values.size());
    for (const auto& reference : impl_->current.conflict_values) {
        if (reference.id.alg == algorithm{}) {
            co_return std::unexpected(
                failure(errc::unsupported, "legacy conflict requires a new merge", reference.path));
        }
        auto encoded = co_await impl_->objects.get(reference.id);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto value = objects::decode_conflict(*encoded, impl_->hash_algorithm);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        output.push_back(std::move(*value));
    }
    co_return output;
}
runtime::task<conflict> repository::conflict_at(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto found =
        std::ranges::find(impl_->current.conflict_values, path, &conflict_reference::path);
    if (found == impl_->current.conflict_values.end()) {
        co_return std::unexpected(failure(errc::not_found, "conflict does not exist", path));
    }
    if (found->id.alg == algorithm{}) {
        co_return std::unexpected(
            failure(errc::unsupported, "legacy conflict requires a new merge", path));
    }
    auto encoded = co_await impl_->objects.get(found->id);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return objects::decode_conflict(*encoded, impl_->hash_algorithm);
}
runtime::task<digest> repository::record_conflict(conflict value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(value.region.path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    if (impl_->current.root_is_paged) {
        auto loaded = co_await impl_->load_paged_root();
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
    }
    const auto* entry = lookup(impl_->current.root, *parts);
    if (!entry) {
        co_return std::unexpected(
            failure(errc::not_found, "conflict path does not exist", value.region.path));
    }
    const auto path = value.region.path;
    auto stored = co_await impl_->store_conflict_value(std::move(value));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation) {
        co_return std::unexpected(operation.error());
    }
    impl_->record_operation(*operation, "conflict", path, {}, entry->node, std::nullopt);
    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto signed_drip = co_await impl_->append_drip("conflict", entry->node, at, {path}, {}, {});
    if (!signed_drip) {
        co_return std::unexpected(signed_drip.error());
    }
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return *stored;
}
runtime::task<change_id> repository::resolve_conflict(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (impl_->current.root_is_paged) {
        auto loaded = co_await impl_->load_paged_root();
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    const auto* entry = lookup(impl_->current.root, *parts);
    const auto found =
        std::ranges::find(impl_->current.conflict_values, path, &conflict_reference::path);
    if (!entry || found == impl_->current.conflict_values.end()) {
        co_return std::unexpected(failure(errc::not_found, "conflict does not exist", path));
    }
    const auto reference = *found;
    impl_->current.conflict_values.erase(found);
    impl_->current.pending_resolutions.push_back({reference.id, entry->object});
    auto resolved = co_await cut("resolve conflict at " + path);
    if (!resolved) {
        impl_->current.pending_resolutions.clear();
        impl_->current.conflict_values.push_back(reference);
        co_return std::unexpected(resolved.error());
    }
    co_return *resolved;
}
runtime::task<change_id> repository::resolve_conflict(digest conflict_id,
                                                      conflict_resolution_request resolution) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (impl_->current.root_is_paged) {
        auto loaded = co_await impl_->load_paged_root();
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
    }
    const auto found =
        std::ranges::find_if(impl_->current.conflict_values, [&](const conflict_reference& value) {
            return same_digest(value.id, conflict_id);
        });
    if (found == impl_->current.conflict_values.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "conflict does not exist", to_string(conflict_id)));
    }
    auto encoded = co_await impl_->objects.get(found->id);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto value = objects::decode_conflict(*encoded);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto contents = std::vector<std::byte>{};
    if (resolution.kind == conflict_resolution_kind::blob) {
        contents = std::move(resolution.blob);
    } else {
        auto reference = std::optional<digest>{};
        if (resolution.kind == conflict_resolution_kind::accept_proposal) {
            reference = value->proposal;
            if (!reference) {
                co_return std::unexpected(
                    failure(errc::invalid_argument, "conflict has no merge proposal", found->path));
            }
        } else if (resolution.kind == conflict_resolution_kind::left) {
            reference = value->left;
        } else if (resolution.kind == conflict_resolution_kind::right) {
            reference = value->right;
        } else {
            co_return std::unexpected(
                failure(errc::invalid_argument, "unknown conflict resolution", found->path));
        }
        auto loaded = co_await impl_->blob_contents(*reference, found->path);
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        contents = std::move(*loaded);
    }
    auto parts = split_path(found->path);
    const auto* entry = parts ? lookup(impl_->current.root, *parts) : nullptr;
    if (!parts || !entry || entry->kind != entry_kind::file) {
        co_return std::unexpected(
            parts ? failure(errc::not_found, "conflict file is absent", found->path)
                  : parts.error());
    }
    const auto path = found->path;
    auto written = co_await write(path, std::move(contents), {.mode = entry->mode});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return co_await resolve_conflict(path);
}
runtime::task<std::vector<operation_record>> repository::operations(log_options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto output = std::vector<operation_record>{};
    output.reserve(impl_->current.operations.size());
    auto verifier = operation_verifier{impl_->current, impl_->signing_key.identity()};
    for (auto operation = impl_->current.operations.rbegin();
         operation != impl_->current.operations.rend();
         ++operation) {
        output.push_back({.id = operation->id,
                          .kind = operation->kind,
                          .from = operation->from,
                          .to = operation->to,
                          .by = operation->by,
                          .at = operation->at,
                          .session = operation->session,
                          .metadata = operation->metadata,
                          .identity_verified = verifier.verified(*operation)});
    }
    co_return output;
}

namespace {

struct attribution_operation_group {
    std::vector<const stored_operation*> operations;
    const stored_operation* record{};
};

struct attribution_source {
    actor by;
    session_id session;
    bool agent{};
    // Set from the change or operation the line came from (liquid#508).
    bool identity_verified{};
};

auto attribution_groups(const repository_document& document)
    -> std::map<std::string, attribution_operation_group> {
    auto output = std::map<std::string, attribution_operation_group>{};
    auto pending = std::vector<const stored_operation*>{};
    for (const auto& operation : document.operations) {
        if (operation.kind != "record" || !operation.head_after) {
            pending.push_back(&operation);
            continue;
        }
        output.insert_or_assign(to_string(operation.head_after->value),
                                attribution_operation_group{pending, &operation});
        pending.clear();
    }
    return output;
}

auto operation_affects(const stored_operation& operation,
                       const node_id& node,
                       std::string_view old_path,
                       std::string_view new_path) -> bool {
    if (operation.node == node || operation.from == old_path || operation.from == new_path ||
        operation.to == old_path || operation.to == new_path) {
        return true;
    }
    return std::ranges::any_of(operation.entries, [&](const stored_entry_effect& effect) {
        const auto same_node = [&](const std::optional<working_entry>& entry) {
            return entry && entry->node == node;
        };
        return effect.path == old_path || effect.path == new_path || same_node(effect.before) ||
               same_node(effect.after);
    });
}

auto content_operation(const stored_operation& operation) -> bool {
    return !operation.entries.empty() || operation.kind == "write" || operation.kind == "import" ||
           operation.kind == "structural_edit" || operation.kind == "undo" ||
           operation.kind == "redaction";
}

auto source_for_change(const std::map<std::string, attribution_operation_group>& groups,
                       operation_verifier& verifier,
                       const change& value,
                       const node_id& node,
                       std::string_view old_path,
                       std::string_view new_path) -> attribution_source {
    const auto from_change =
        attribution_source{.by = value.author, .identity_verified = value.identity_verified};
    const auto found = groups.find(to_string(value.id.value));
    if (found == groups.end()) {
        return from_change;
    }
    const auto& operations = found->second.operations;
    auto selected = static_cast<const stored_operation*>(nullptr);
    for (auto operation = operations.rbegin(); operation != operations.rend(); ++operation) {
        if (content_operation(**operation) &&
            operation_affects(**operation, node, old_path, new_path)) {
            selected = *operation;
            break;
        }
    }
    if (!selected) {
        for (auto operation = operations.rbegin(); operation != operations.rend(); ++operation) {
            if (content_operation(**operation)) {
                selected = *operation;
                break;
            }
        }
    }
    selected = selected ? selected : found->second.record;
    return selected ? attribution_source{.by = selected->by,
                                         .session = selected->session,
                                         .agent = selected->agent,
                                         .identity_verified = verifier.verified(*selected)}
                    : from_change;
}

auto text_lines(std::span<const std::byte> bytes) -> std::vector<std::string> {
    const auto source = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    auto output = std::vector<std::string>{};
    for (auto offset = std::size_t{}; offset < source.size();) {
        const auto newline = source.find('\n', offset);
        const auto end = newline == std::string_view::npos ? source.size() : newline;
        output.emplace_back(source.substr(offset, end - offset));
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return output;
}

auto apply_attribution_hunks(std::span<const line_attribution> previous,
                             std::span<const std::string> lines,
                             std::span<const line_change> hunks,
                             const attribution_source& source,
                             const change_id& change_value)
    -> result<std::vector<line_attribution>> {
    auto output = std::vector<line_attribution>{};
    output.reserve(lines.size());
    auto old_offset = std::size_t{};
    auto new_offset = std::size_t{};
    for (const auto& hunk : hunks) {
        if (hunk.old_line == 0 || hunk.new_line == 0) {
            return std::unexpected(failure(errc::corrupt, "invalid attribution line hunk"));
        }
        const auto old_start = static_cast<std::size_t>(hunk.old_line - 1);
        const auto new_start = static_cast<std::size_t>(hunk.new_line - 1);
        if (old_start < old_offset || new_start < new_offset || old_start > previous.size() ||
            new_start > lines.size() || old_start - old_offset != new_start - new_offset ||
            hunk.old_count > previous.size() - old_start ||
            hunk.new_count > lines.size() - new_start) {
            return std::unexpected(failure(errc::corrupt, "inconsistent attribution line hunk"));
        }
        output.insert(output.end(),
                      previous.begin() + static_cast<std::ptrdiff_t>(old_offset),
                      previous.begin() + static_cast<std::ptrdiff_t>(old_start));
        for (auto index = std::size_t{}; index < hunk.new_count; ++index) {
            output.push_back({.text = lines[new_start + index],
                              .by = source.by,
                              .session = source.session,
                              .change = change_value,
                              .agent = source.agent,
                              .identity_verified = source.identity_verified});
        }
        old_offset = old_start + static_cast<std::size_t>(hunk.old_count);
        new_offset = new_start + static_cast<std::size_t>(hunk.new_count);
    }
    output.insert(
        output.end(), previous.begin() + static_cast<std::ptrdiff_t>(old_offset), previous.end());
    if (output.size() != lines.size()) {
        return std::unexpected(failure(errc::corrupt, "attribution line count does not match"));
    }
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        output[index].line = index + 1;
        output[index].text = lines[index];
    }
    return output;
}

auto snapshot_entry(repository& source, liquid::tree current, std::span<const std::string> path)
    -> runtime::task<tree_entry> {
    for (auto index = std::size_t{}; index < path.size(); ++index) {
        const auto found = std::ranges::find(current.entries, path[index], &tree_entry::name);
        if (found == current.entries.end()) {
            co_return std::unexpected(failure(errc::not_found, "path does not exist", path[index]));
        }
        if (index + 1 == path.size()) {
            if (found->kind != node_kind::blob) {
                co_return std::unexpected(
                    failure(errc::invalid_argument, "attribution path is not a file", found->name));
            }
            co_return *found;
        }
        if (found->kind != node_kind::tree) {
            co_return std::unexpected(failure(errc::not_found, "path does not exist", path[index]));
        }
        auto child = co_await source.tree(found->ref);
        if (!child) {
            co_return std::unexpected(child.error());
        }
        current = std::move(*child);
    }
    co_return std::unexpected(failure(errc::invalid_argument, "attribution path is empty"));
}

} // namespace

runtime::task<std::vector<line_attribution>> repository::attribution(std::string path,
                                                                     change_id change_value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    auto target = co_await at(change_value);
    if (!target) {
        co_return std::unexpected(target.error());
    }
    auto target_entry = co_await snapshot_entry(*this, target->root, *parts);
    if (!target_entry) {
        co_return std::unexpected(target_entry.error());
    }
    auto history = co_await log({.from = change_value, .first_parent = true});
    if (!history) {
        co_return std::unexpected(history.error());
    }
    std::ranges::reverse(*history);
    const auto groups = attribution_groups(impl_->current);
    auto verifier = operation_verifier{impl_->current, impl_->signing_key.identity()};
    auto output = std::vector<line_attribution>{};
    auto previous = std::optional<change_id>{};
    for (const auto& revision : *history) {
        auto differences = result<std::vector<tree_diff_entry>>{};
        if (previous) {
            differences = co_await diff(*previous, revision.id, {.text = true});
        } else {
            auto snapshot = co_await at(revision.id);
            if (!snapshot) {
                co_return std::unexpected(snapshot.error());
            }
            differences = co_await diff(liquid::tree{}, std::move(snapshot->root), {.text = true});
        }
        if (!differences) {
            co_return std::unexpected(differences.error());
        }
        previous = revision.id;
        const auto changed =
            std::ranges::find(*differences, target_entry->node, &tree_diff_entry::node);
        if (changed == differences->end()) {
            continue;
        }
        if (!changed->new_blob) {
            output.clear();
            continue;
        }
        if (!changed->line_hunks) {
            if (changed->old_blob && changed->new_blob &&
                same_digest(*changed->old_blob, *changed->new_blob)) {
                continue;
            }
            co_return std::unexpected(
                failure(errc::unsupported, "attribution requires UTF-8 text", changed->path));
        }
        auto contents = co_await blob(*changed->new_blob);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        auto lines = text_lines(*contents);
        const auto source = source_for_change(groups,
                                              verifier,
                                              revision,
                                              target_entry->node,
                                              changed->old_path.value_or(changed->path),
                                              changed->path);
        auto attributed =
            apply_attribution_hunks(output, lines, *changed->line_hunks, source, revision.id);
        if (!attributed) {
            co_return std::unexpected(attributed.error());
        }
        output = std::move(*attributed);
    }
    co_return output;
}

runtime::task<undo_report> repository::undo(op_id operation) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto found = std::ranges::find_if(impl_->current.operations, [&](const auto& value) {
        return value.id.value.alg == operation.value.alg &&
               value.id.value.bytes == operation.value.bytes;
    });
    if (found == impl_->current.operations.end())
        co_return std::unexpected(failure(errc::not_found, "operation does not exist"));
    if (found->entries.empty() && found->refs.empty() && found->locks.empty())
        co_return std::unexpected(failure(errc::unsupported, "operation cannot be undone"));
    co_return co_await impl_->apply_undo({*found}, {});
}
runtime::task<undo_report> repository::undo_session(session_id session, undo_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto selected = std::vector<stored_operation>{};
    for (auto operation = impl_->current.operations.rbegin();
         operation != impl_->current.operations.rend();
         ++operation) {
        if (operation->session == session && selected_drip(operation->id, options)) {
            selected.push_back(*operation);
        }
    }
    if (selected.empty()) {
        co_return std::unexpected(
            failure(errc::not_found, "session has no matching operations", to_string(session)));
    }
    co_return co_await impl_->apply_undo(std::move(selected), std::move(options));
}

struct session::impl {
    repository* shared{};
    repository private_replica;
    digest base_tree{};
    change_id base_change{};
    journal_cursor base_cursor;
    actor agent;
    attestation delegation;
    digest delegation_id{};
    std::vector<std::string> scopes;
    bool may_publish{};
    bool may_lease{};
    bool active{true};
};

namespace {

struct stored_session {
    actor agent;
    attestation delegation;
    digest base_tree{};
    change_id base_change{};
    journal_cursor base_cursor;
};

auto encode_stored_session(const stored_session& value) -> result<std::vector<std::byte>> {
    auto delegation = encode_attestation(value.delegation);
    if (!delegation) {
        return std::unexpected(delegation.error());
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"agent", actor_value(value.agent)},
                             {"delegation", bytes(*delegation)},
                             {"base_tree", digest_value(value.base_tree)},
                             {"base_change", digest_value(value.base_change.value)},
                             {"base_cursor", cbor::value{value.base_cursor.token}}}));
}

auto decode_stored_session(std::span<const std::byte> encoded) -> result<stored_session> {
    auto value = cbor::decode(encoded);
    const auto* version_value = value ? find(*value, "v") : nullptr;
    const auto* agent_value = value ? find(*value, "agent") : nullptr;
    const auto* delegation_value = value ? find(*value, "delegation") : nullptr;
    const auto* tree_value = value ? find(*value, "base_tree") : nullptr;
    const auto* change_value = value ? find(*value, "base_change") : nullptr;
    const auto* cursor_value = value ? find(*value, "base_cursor") : nullptr;
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* delegation_bytes =
        delegation_value ? std::get_if<cbor::value::bytes>(&delegation_value->data) : nullptr;
    const auto* cursor = cursor_value ? std::get_if<std::string>(&cursor_value->data) : nullptr;
    if (!value || !version || *version != 1 || !agent_value || !delegation_bytes || !tree_value ||
        !change_value || !cursor) {
        return std::unexpected(failure(errc::corrupt, "invalid session metadata"));
    }
    auto agent = decode_actor(*agent_value);
    auto delegation = decode_attestation(*delegation_bytes);
    auto tree = decode_digest(*tree_value);
    auto change = decode_digest(*change_value);
    if (!agent || !delegation || !tree || !change) {
        return std::unexpected(!agent        ? agent.error()
                               : !delegation ? delegation.error()
                               : !tree       ? tree.error()
                                             : change.error());
    }
    return stored_session{.agent = std::move(*agent),
                          .delegation = std::move(*delegation),
                          .base_tree = *tree,
                          .base_change = change_id{*change},
                          .base_cursor = journal_cursor{*cursor}};
}

auto delegation_attribute(const attestation& value, std::string_view name)
    -> const attestation_value* {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    return found == value.attributes.end() ? nullptr : &found->value;
}

auto session_path_allowed(std::span<const std::string> scopes, std::string_view path) -> bool {
    return std::ranges::any_of(scopes,
                               [&](const auto& scope) { return glob_matches(scope, path); });
}

} // namespace

runtime::task<session> repository::fork_session(session_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (!impl_->current.head) {
        co_return std::unexpected(
            failure(errc::not_found, "a session requires a recorded fork point"));
    }
    auto delegation_valid = validate_delegation(options.delegation);
    if (!delegation_valid) {
        co_return std::unexpected(delegation_valid.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto verified = verify_attestation(
        options.delegation,
        {.now = now, .status = attestation_status{.revoked = false, .checked_at = now}});
    if (!verified || !verified->verified) {
        co_return std::unexpected(failure(errc::unauthorized,
                                          verified ? verified->reason : verified.error().message,
                                          "delegation/1"));
    }
    if (options.delegation.issuer.taap_id != impl_->signing_key.identity().taap_id) {
        co_return std::unexpected(
            failure(errc::unauthorized, "delegation was not issued by this principal"));
    }
    const auto* scopes =
        std::get_if<std::vector<std::string>>(delegation_attribute(options.delegation, "scopes"));
    const auto* may_publish =
        std::get_if<bool>(delegation_attribute(options.delegation, "may_publish"));
    const auto* may_lease =
        std::get_if<bool>(delegation_attribute(options.delegation, "may_lease"));
    if (!scopes || scopes->empty() || !may_publish || !may_lease) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "delegation has no usable authority"));
    }
    const auto delegated_scopes = *scopes;
    const auto delegated_publish = *may_publish;
    const auto delegated_lease = *may_lease;
    auto encoded_delegation = encode_attestation(options.delegation);
    if (!encoded_delegation) {
        co_return std::unexpected(encoded_delegation.error());
    }
    auto delegation_id = co_await repository_detail::store_access::put_plain(
        impl_->objects, std::move(*encoded_delegation));
    if (!delegation_id) {
        co_return std::unexpected(delegation_id.error());
    }
    auto encoded = encode_repository_document(impl_->current);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto document = decode_repository_document(*encoded);
    if (!document) {
        co_return std::unexpected(document.error());
    }
    document->operations.clear();
    document->drips.clear();
    document->leases.clear();
    document->pending_drips = 0;
    const auto id = session_id{random_id()};
    auto private_path = impl_->root_path / "sessions" / to_string(id);
    auto private_state = impl_->state.child("sessions/" + to_string(id));
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key) {
        co_return std::unexpected(signing_key.error());
    }
    auto private_repository = repository{};
    private_repository.impl_ = std::make_unique<repository::impl>(private_path,
                                                                  private_state,
                                                                  nullptr,
                                                                  impl_->objects,
                                                                  std::move(*document),
                                                                  std::move(*signing_key),
                                                                  impl_->keys,
                                                                  impl_->checkpoint_interval,
                                                                  impl_->hash_algorithm,
                                                                  id);
    if (private_repository.impl_->current.root_is_paged) {
        auto restored = co_await private_repository.impl_->load_paged_root();
        if (!restored) {
            co_return std::unexpected(restored.error());
        }
    }
    private_repository.impl_->acting_actor = options.actor;
    private_repository.impl_->acting_delegation = *delegation_id;
    auto checkpointed = co_await private_repository.impl_->checkpoint();
    if (!checkpointed) {
        co_return std::unexpected(checkpointed.error());
    }
    auto base_tree = co_await root_digest();
    if (!base_tree) {
        co_return std::unexpected(base_tree.error());
    }
    auto implementation = std::make_unique<session::impl>();
    implementation->shared = this;
    implementation->private_replica = std::move(private_repository);
    implementation->base_tree = *base_tree;
    implementation->base_change = *impl_->current.head;
    implementation->base_cursor = journal_cursor{std::to_string(impl_->current.drips.size())};
    implementation->agent = std::move(options.actor);
    implementation->delegation = std::move(options.delegation);
    implementation->delegation_id = *delegation_id;
    implementation->scopes = delegated_scopes;
    implementation->may_publish = delegated_publish;
    implementation->may_lease = delegated_lease;
    auto metadata = encode_stored_session({.agent = implementation->agent,
                                           .delegation = implementation->delegation,
                                           .base_tree = implementation->base_tree,
                                           .base_change = implementation->base_change,
                                           .base_cursor = implementation->base_cursor});
    if (!metadata) {
        co_return std::unexpected(metadata.error());
    }
    auto metadata_written =
        co_await publish_state(private_state, "LIQUID_SESSION", std::move(*metadata));
    if (!metadata_written) {
        co_return std::unexpected(metadata_written.error());
    }
    co_return session{std::move(implementation)};
}

runtime::task<session> repository::open_session(session_id id) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto private_path = impl_->root_path / "sessions" / to_string(id);
    const auto private_state = impl_->state.child("sessions/" + to_string(id));
    auto metadata_bytes = co_await read_state(private_state, "LIQUID_SESSION");
    if (!metadata_bytes) {
        co_return std::unexpected(metadata_bytes.error());
    }
    if (!*metadata_bytes) {
        co_return std::unexpected(failure(
            errc::not_found, "session does not exist", private_state.key("LIQUID_SESSION")));
    }
    auto metadata = decode_stored_session(**metadata_bytes);
    if (!metadata) {
        co_return std::unexpected(metadata.error());
    }
    auto document = co_await load_state_document(private_state, impl_->objects);
    if (!document) {
        co_return std::unexpected(document.error());
    }
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key) {
        co_return std::unexpected(signing_key.error());
    }
    auto private_repository = repository{};
    private_repository.impl_ = std::make_unique<repository::impl>(private_path,
                                                                  private_state,
                                                                  nullptr,
                                                                  impl_->objects,
                                                                  std::move(*document),
                                                                  std::move(*signing_key),
                                                                  impl_->keys,
                                                                  impl_->checkpoint_interval,
                                                                  impl_->hash_algorithm,
                                                                  id);
    if (private_repository.impl_->current.root_is_paged) {
        auto restored = co_await private_repository.impl_->load_paged_root();
        if (!restored) {
            co_return std::unexpected(restored.error());
        }
    }
    private_repository.impl_->acting_actor = metadata->agent;
    auto encoded_delegation = encode_attestation(metadata->delegation);
    if (!encoded_delegation) {
        co_return std::unexpected(encoded_delegation.error());
    }
    auto delegation_id = co_await repository_detail::store_access::put_plain(
        impl_->objects, std::move(*encoded_delegation));
    if (!delegation_id) {
        co_return std::unexpected(delegation_id.error());
    }
    private_repository.impl_->acting_delegation = *delegation_id;
    const auto* scopes =
        std::get_if<std::vector<std::string>>(delegation_attribute(metadata->delegation, "scopes"));
    const auto* may_publish =
        std::get_if<bool>(delegation_attribute(metadata->delegation, "may_publish"));
    const auto* may_lease =
        std::get_if<bool>(delegation_attribute(metadata->delegation, "may_lease"));
    if (!scopes || !may_publish || !may_lease) {
        co_return std::unexpected(failure(errc::corrupt, "session authority is incomplete"));
    }
    const auto delegated_scopes = *scopes;
    const auto delegated_publish = *may_publish;
    const auto delegated_lease = *may_lease;
    auto implementation = std::make_unique<session::impl>();
    implementation->shared = this;
    implementation->private_replica = std::move(private_repository);
    implementation->base_tree = metadata->base_tree;
    implementation->base_change = metadata->base_change;
    implementation->base_cursor = metadata->base_cursor;
    implementation->agent = std::move(metadata->agent);
    implementation->delegation = std::move(metadata->delegation);
    implementation->delegation_id = *delegation_id;
    implementation->scopes = delegated_scopes;
    implementation->may_publish = delegated_publish;
    implementation->may_lease = delegated_lease;
    co_return session{std::move(implementation)};
}

session::session() noexcept = default;
session::~session() = default;
session::session(session&&) noexcept = default;
auto session::operator=(session&&) noexcept -> session& = default;
session::session(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

session_id session::id() const noexcept {
    return impl_ ? impl_->private_replica.impl_->session : session_id{};
}

runtime::task<std::vector<std::byte>> session::read(std::string path) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    co_return co_await impl_->private_replica.read(std::move(path));
}

runtime::task<void> session::write(std::string path, std::vector<std::byte> contents) {
    co_return co_await write(std::move(path), std::move(contents), {});
}

runtime::task<void>
session::write(std::string path, std::vector<std::byte> contents, write_options options) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    options.base.reset();
    co_return co_await impl_->private_replica.write(
        std::move(path), std::move(contents), std::move(options));
}

runtime::task<void>
session::write_file(std::string path, std::filesystem::path source, write_options options) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    options.base.reset();
    co_return co_await impl_->private_replica.write_file(
        std::move(path), std::move(source), std::move(options));
}

runtime::task<void> session::move(std::string from, std::string to, write_options options) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    options.base.reset();
    co_return co_await impl_->private_replica.move(
        std::move(from), std::move(to), std::move(options));
}

runtime::task<void> session::remove(std::string path, write_options options) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    options.base.reset();
    co_return co_await impl_->private_replica.remove(std::move(path), std::move(options));
}

runtime::task<void> session::lease(lease_claim claim, std::chrono::seconds ttl) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    if (!impl_->may_lease) {
        co_return std::unexpected(
            failure(errc::unauthorized, "delegation does not permit leases", claim.path));
    }
    if (!session_path_allowed(impl_->scopes, claim.path)) {
        co_return std::unexpected(
            failure(errc::unauthorized, "lease is outside delegation scopes", claim.path));
    }
    auto& shared = *impl_->shared->impl_;
    const auto saved_session = shared.session;
    shared.session = id();
    shared.acting_actor = impl_->agent;
    shared.acting_delegation = impl_->delegation_id;
    auto claimed = co_await impl_->shared->lease(std::move(claim), ttl);
    shared.acting_delegation.reset();
    shared.acting_actor.reset();
    shared.session = saved_session;
    co_return claimed;
}

runtime::task<void> session::release(lease_claim claim) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    auto& shared = *impl_->shared->impl_;
    const auto saved_session = shared.session;
    shared.session = id();
    shared.acting_actor = impl_->agent;
    shared.acting_delegation = impl_->delegation_id;
    auto released = co_await impl_->shared->release(std::move(claim));
    shared.acting_delegation.reset();
    shared.acting_actor.reset();
    shared.session = saved_session;
    co_return released;
}

runtime::task<void> session::report_presence(liquid::presence value) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    value.actor = impl_->agent;
    value.session = id();
    co_return co_await impl_->shared->report_presence(std::move(value));
}

runtime::task<publish_report> session::publish(publish_options options) {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    if (!impl_->may_publish) {
        co_return std::unexpected(
            failure(errc::unauthorized, "delegation does not permit publishing"));
    }
    auto private_tree = co_await impl_->private_replica.root_digest();
    if (!private_tree) {
        co_return std::unexpected(private_tree.error());
    }
    auto differences = co_await impl_->shared->diff(impl_->base_tree, *private_tree);
    if (!differences) {
        co_return std::unexpected(differences.error());
    }
    auto paths = std::vector<std::string>{};
    for (const auto& entry : *differences) {
        if (entry.kind != node_kind::blob) {
            continue;
        }
        if (entry.old_path) {
            paths.push_back(*entry.old_path);
        }
        paths.push_back(entry.path);
    }
    std::ranges::sort(paths);
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const auto& path : paths) {
        if (!session_path_allowed(impl_->scopes, path)) {
            co_return std::unexpected(
                failure(errc::unauthorized, "publish is outside delegation scopes", path));
        }
    }
    auto base_options = write_options{.base = write_base{impl_->base_change, impl_->base_cursor}};
    auto& shared = *impl_->shared->impl_;
    const auto saved_session = shared.session;
    shared.session = id();
    shared.acting_actor = impl_->agent;
    shared.acting_delegation = impl_->delegation_id;
    auto base_valid = co_await impl_->shared->validate_write(base_options, paths);
    if (!base_valid) {
        shared.acting_delegation.reset();
        shared.acting_actor.reset();
        shared.session = saved_session;
        co_return std::unexpected(base_valid.error());
    }
    for (const auto& path : paths) {
        auto lease_valid = shared.validate_lease_write(path, {});
        if (!lease_valid) {
            shared.acting_delegation.reset();
            shared.acting_actor.reset();
            shared.session = saved_session;
            co_return std::unexpected(lease_valid.error());
        }
    }
    auto backup_bytes = encode_repository_document(shared.current);
    if (!backup_bytes) {
        shared.acting_delegation.reset();
        shared.acting_actor.reset();
        shared.session = saved_session;
        co_return std::unexpected(backup_bytes.error());
    }
    auto document_backups = std::map<node_id, std::vector<std::byte>>{};
    for (auto& [node, document] : shared.directory_cache) {
        auto saved = document.save_full();
        if (!saved) {
            shared.acting_delegation.reset();
            shared.acting_actor.reset();
            shared.session = saved_session;
            co_return std::unexpected(saved.error());
        }
        document_backups.emplace(node, std::move(*saved));
    }
    const auto restore_shared = [&]() -> result<void> {
        auto restored = decode_repository_document(*backup_bytes);
        if (!restored) {
            return std::unexpected(restored.error());
        }
        shared.current = std::move(*restored);
        shared.directory_cache.clear();
        for (const auto& [node, saved] : document_backups) {
            auto loaded = crdt::directory_document::load_sync(
                saved, "local", shared.root_path.generic_string());
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
            shared.directory_cache.emplace(node, std::move(*loaded));
        }
        return {};
    };
    const auto reset_publish_state = [&]() {
        shared.defer_persistence = false;
        shared.suppress_drips = false;
        shared.suppressed_drips = 0;
        shared.acting_delegation.reset();
        shared.acting_actor.reset();
        shared.session = saved_session;
    };
    auto report = publish_report{.session = id()};
    auto touched_directories = std::set<std::string>{};
    auto directory_paths = std::map<std::string, std::vector<std::string>>{};
    shared.defer_persistence = true;
    shared.suppress_drips = true;
    shared.suppressed_drips = 0;
    for (const auto& entry : *differences) {
        if (entry.kind != node_kind::blob) {
            continue;
        }
        const auto slash = entry.path.rfind('/');
        const auto directory =
            slash == std::string::npos ? std::string{} : entry.path.substr(0, slash);
        touched_directories.insert(directory);
        auto applied = result<void>{};
        if (entry.old_path && *entry.old_path != entry.path) {
            applied = co_await impl_->shared->remove(*entry.old_path);
        }
        auto contents = co_await impl_->private_replica.read(entry.path);
        if (applied && contents) {
            auto write = write_options{};
            write.mode = entry.new_mode.value_or(0100644);
            applied = co_await impl_->shared->write(entry.path, std::move(*contents), write);
        } else if (applied && contents.error().code == errc::not_found) {
            applied = co_await impl_->shared->remove(entry.path);
        } else if (applied) {
            applied = std::unexpected(contents.error());
        }
        report.paths.push_back({.path = entry.path,
                                .applied = applied.has_value(),
                                .reason = applied ? "" : applied.error().message});
        if (applied) {
            if (entry.old_path && *entry.old_path != entry.path) {
                const auto old_slash = entry.old_path->rfind('/');
                const auto old_directory = old_slash == std::string::npos
                                               ? std::string{}
                                               : entry.old_path->substr(0, old_slash);
                touched_directories.insert(old_directory);
                directory_paths[old_directory].push_back(*entry.old_path);
            }
            directory_paths[directory].push_back(entry.path);
        }
        if (!applied && options.atomic) {
            auto restored = restore_shared();
            if (!restored) {
                reset_publish_state();
                co_return std::unexpected(restored.error());
            }
            auto checkpointed = co_await shared.checkpoint();
            reset_publish_state();
            if (!checkpointed) {
                co_return std::unexpected(checkpointed.error());
            }
            co_return std::unexpected(applied.error());
        }
    }
    shared.defer_persistence = false;
    shared.suppress_drips = false;
    if (shared.current.pending_drips >= shared.suppressed_drips) {
        shared.current.pending_drips -= shared.suppressed_drips;
    }
    shared.suppressed_drips = 0;
    const auto publish_time =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (auto& [directory, changed_paths] : directory_paths) {
        auto parts = split_path(changed_paths.front());
        if (!parts) {
            auto restored = restore_shared();
            auto checkpointed = restored ? co_await shared.checkpoint() : result<void>{};
            reset_publish_state();
            if (!restored) {
                co_return std::unexpected(restored.error());
            }
            if (!checkpointed) {
                co_return std::unexpected(checkpointed.error());
            }
            co_return std::unexpected(parts.error());
        }
        auto* parent = parent_for(shared.current.root, *parts, false, shared.hash_algorithm)
                           .value_or(&shared.current.root);
        std::ranges::sort(changed_paths);
        changed_paths.erase(std::unique(changed_paths.begin(), changed_paths.end()),
                            changed_paths.end());
        auto appended = co_await shared.append_drip(
            "publish", parent->node, publish_time, std::move(changed_paths), {}, std::nullopt);
        if (!appended) {
            auto restored = restore_shared();
            auto checkpointed = restored ? co_await shared.checkpoint() : result<void>{};
            reset_publish_state();
            if (!restored) {
                co_return std::unexpected(restored.error());
            }
            if (!checkpointed) {
                co_return std::unexpected(checkpointed.error());
            }
            co_return std::unexpected(appended.error());
        }
        ++shared.current.pending_drips;
        static_cast<void>(directory);
    }
    report.directories = touched_directories.size();
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (auto& value : shared.current.leases) {
        if (value.by.taap_id == impl_->agent.taap_id && value.session == id() &&
            value.expires > now) {
            value.expires = now + (value.expires - value.since);
            value.since = now;
        }
    }
    if (options.message) {
        auto change = co_await impl_->shared->cut(*options.message);
        if (!change) {
            auto restored = restore_shared();
            auto checkpointed = restored ? co_await shared.checkpoint() : result<void>{};
            reset_publish_state();
            if (!restored) {
                co_return std::unexpected(restored.error());
            }
            if (!checkpointed) {
                co_return std::unexpected(checkpointed.error());
            }
            co_return std::unexpected(change.error());
        }
        report.change = *change;
    } else {
        auto checkpointed = co_await shared.checkpoint_or_journal();
        if (!checkpointed) {
            auto restored = restore_shared();
            auto rollback_checkpointed = restored ? co_await shared.checkpoint() : result<void>{};
            reset_publish_state();
            if (!restored) {
                co_return std::unexpected(restored.error());
            }
            if (!rollback_checkpointed) {
                co_return std::unexpected(rollback_checkpointed.error());
            }
            co_return std::unexpected(checkpointed.error());
        }
    }
    reset_publish_state();
    auto removed = co_await remove_state_tree(impl_->private_replica.impl_->state);
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    impl_->active = false;
    co_return report;
}

runtime::task<void> session::discard() {
    if (!impl_ || !impl_->active) {
        co_return std::unexpected(failure(errc::invalid_argument, "session is not active"));
    }
    auto owned = std::vector<lease_claim>{};
    for (const auto& value : impl_->shared->impl_->current.leases) {
        if (value.by.taap_id == impl_->agent.taap_id && value.session == id()) {
            owned.push_back(value.claim);
        }
    }
    for (auto& claim : owned) {
        auto released = co_await release(std::move(claim));
        if (!released) {
            co_return std::unexpected(released.error());
        }
    }
    auto removed = co_await remove_state_tree(impl_->private_replica.impl_->state);
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    impl_->active = false;
    co_return result<void>{};
}
runtime::task<void> repository::lock(std::string path, std::string reason) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    if (reason.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "lock reason must not be empty", path));
    }
    if (!impl_->lock_rule_matches(path)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "path does not match a configured lock rule", path));
    }
    auto allowed = impl_->validate_lock_acquire(path);
    if (!allowed) {
        co_return std::unexpected(allowed.error());
    }
    const auto existing = std::ranges::find(impl_->current.locks, path, &exclusive_lock::path);
    if (existing != impl_->current.locks.end()) {
        if (existing->holder.taap_id == impl_->current_actor().taap_id) {
            co_return result<void>{};
        }
        auto allowed = impl_->validate_lock_write(path);
        co_return std::unexpected(allowed.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto value = exclusive_lock{.path = path,
                                .holder = impl_->current_actor(),
                                .since = now,
                                .reason = std::move(reason),
                                .authority = impl_->authorize_lock ? lock_authority::lake
                                                                   : lock_authority::advisory};
    if (impl_->authorize_lock) {
        auto authorized = co_await impl_->authorize_lock({lock_operation::acquire, value});
        if (!authorized) {
            co_return std::unexpected(authorized.error());
        }
    }
    auto appended = co_await impl_->append_lock_drip("lock", value);
    if (!appended) {
        co_return std::unexpected(appended.error());
    }
    impl_->current.locks.push_back(value);
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "lock";
    operation.from = path;
    operation.locks.push_back({path, false, true});
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}
runtime::task<void> repository::unlock(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto found = std::ranges::find(impl_->current.locks, path, &exclusive_lock::path);
    if (found == impl_->current.locks.end()) {
        co_return std::unexpected(failure(errc::not_found, "path is not locked", path));
    }
    if (!found->holder.taap_id.empty() && found->holder.taap_id != impl_->current_actor().taap_id) {
        co_return std::unexpected(
            failure(errc::unauthorized, "only the lock holder can unlock the path", path));
    }
    const auto value = *found;
    if (value.authority == lock_authority::lake && !impl_->authorize_lock) {
        co_return std::unexpected(
            failure(errc::unavailable, "lake authority is required to release this lock", path));
    }
    if (impl_->authorize_lock) {
        auto authorized = co_await impl_->authorize_lock({lock_operation::release, value});
        if (!authorized) {
            co_return std::unexpected(authorized.error());
        }
    }
    auto appended = co_await impl_->append_lock_drip("unlock", value);
    if (!appended) {
        co_return std::unexpected(appended.error());
    }
    impl_->current.locks.erase(found);
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "unlock";
    operation.from = path;
    operation.locks.push_back({path, true, false});
    impl_->record_operation(std::move(operation));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}

runtime::task<std::vector<exclusive_lock>> repository::locks() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return impl_->current.locks;
}

runtime::task<void> repository::set_lock_configuration(lock_config configuration) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto valid = validate_lock_config(configuration);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    if (impl_->current.lock_rules == configuration) {
        co_return result<void>{};
    }
    auto appended = co_await impl_->append_lock_drip("lock_config", std::nullopt, configuration);
    if (!appended) {
        co_return std::unexpected(appended.error());
    }
    impl_->current.lock_rules = std::move(configuration);
    ++impl_->current.pending_drips;
    co_return co_await impl_->checkpoint_or_journal();
}

lock_config repository::lock_configuration() const {
    return impl_ ? impl_->current.lock_rules : lock_config{};
}

runtime::task<void> repository::lease(lease_claim claim, std::chrono::seconds ttl) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto path = split_path(claim.path);
    if (!path || ttl <= std::chrono::seconds::zero()) {
        co_return std::unexpected(
            path ? failure(errc::invalid_argument, "lease ttl must be positive", claim.path)
                 : path.error());
    }
    if (claim.kind == lease_kind::file) {
        claim.symbol.reset();
        claim.range.reset();
    } else if (claim.kind == lease_kind::range && !claim.range) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "range lease requires a range", claim.path));
    } else if (claim.kind == lease_kind::symbol && (!claim.symbol || claim.symbol->empty())) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "symbol lease requires a symbol", claim.path));
    }
    if (claim.range && claim.range->start.byte >= claim.range->end.byte) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "lease range must not be empty", claim.path));
    }
    if (claim.kind == lease_kind::symbol && !claim.range) {
        auto contents = co_await read(claim.path);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        auto subject = hash(algorithm::blake3, *contents);
        if (!subject) {
            co_return std::unexpected(subject.error());
        }
        auto recorded = co_await symbols(*subject);
        if (!recorded) {
            co_return std::unexpected(recorded.error());
        }
        const auto occurrence = std::ranges::find_if(recorded->occurrences, [&](const auto& value) {
            return value.symbol == *claim.symbol || value.name == *claim.symbol;
        });
        if (occurrence == recorded->occurrences.end()) {
            co_return std::unexpected(
                failure(errc::not_found, "symbol is not recorded", *claim.symbol));
        }
        claim.range = occurrence->range;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto authority =
        impl_->authorize_lease ? lease_authority::lake : lease_authority::advisory;
    if (impl_->authorize_lease) {
        auto authorized = co_await impl_->authorize_lease(
            {claim, impl_->current_actor(), impl_->session, now, now + ttl, lease_authority::lake});
        if (!authorized) {
            co_return std::unexpected(authorized.error());
        }
    }
    std::erase_if(impl_->current.leases, [&](const auto& value) { return value.expires <= now; });
    const auto owned = std::ranges::find_if(impl_->current.leases, [&](const auto& value) {
        return value.by.taap_id == impl_->current_actor().taap_id &&
               value.session == impl_->session && impl_->same_claim(value.claim, claim);
    });
    const auto renewing = owned != impl_->current.leases.end();
    if (renewing) {
        owned->expires = now + ttl;
    } else {
        if (const auto* holder = impl_->lease_holder(claim)) {
            co_return std::unexpected(failure(errc::locked,
                                              "semantic lease is held by " + holder->by.taap_id,
                                              holder->claim.path));
        }
        impl_->current.leases.push_back(
            {std::move(claim), impl_->current_actor(), impl_->session, now, now + ttl, authority});
    }
    const auto& saved_claim = renewing ? owned->claim : impl_->current.leases.back().claim;
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation) {
        co_return std::unexpected(operation.error());
    }
    impl_->record_operation(*operation,
                            "lease",
                            saved_claim.path,
                            saved_claim.symbol.value_or(std::string{}),
                            impl_->current.root.node,
                            std::nullopt);
    auto appended = co_await impl_->append_drip(
        "lease", impl_->current.root.node, now, {saved_claim.path}, {}, std::nullopt);
    if (!appended) {
        co_return std::unexpected(appended.error());
    }
    ++impl_->current.pending_drips;
    co_return co_await impl_->checkpoint_or_journal();
}

runtime::task<void> repository::release(lease_claim claim) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto found = std::ranges::find_if(impl_->current.leases, [&](const auto& value) {
        return impl_->same_claim(value.claim, claim);
    });
    if (found == impl_->current.leases.end()) {
        co_return std::unexpected(failure(errc::not_found, "semantic lease does not exist"));
    }
    if (found->by.taap_id != impl_->current_actor().taap_id || found->session != impl_->session) {
        co_return std::unexpected(
            failure(errc::unauthorized, "semantic lease belongs to another session", claim.path));
    }
    const auto path = found->claim.path;
    impl_->current.leases.erase(found);
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation) {
        co_return std::unexpected(operation.error());
    }
    impl_->record_operation(
        *operation, "release", path, {}, impl_->current.root.node, std::nullopt);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto appended = co_await impl_->append_drip(
        "release", impl_->current.root.node, now, {path}, {}, std::nullopt);
    if (!appended) {
        co_return std::unexpected(appended.error());
    }
    ++impl_->current.pending_drips;
    co_return co_await impl_->checkpoint_or_journal();
}

runtime::task<std::vector<semantic_lease>> repository::leases() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::erase_if(impl_->current.leases, [&](const auto& value) { return value.expires <= now; });
    co_return impl_->current.leases;
}

runtime::task<void> repository::report_presence(liquid::presence value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto path = split_path(value.path);
    if (!path) {
        co_return std::unexpected(path.error());
    }
    if (value.actor.taap_id.empty()) {
        value.actor = impl_->current_actor();
    }
    if (value.session == session_id{}) {
        value.session = impl_->session;
    }
    if (value.at == std::chrono::sys_seconds{}) {
        value.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    }
    std::erase_if(impl_->transient_presence, [&](const auto& current) {
        return current.actor.taap_id == value.actor.taap_id && current.session == value.session;
    });
    impl_->transient_presence.push_back(std::move(value));
    co_return result<void>{};
}

runtime::task<std::vector<liquid::presence>> repository::presence() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    constexpr auto lifetime = std::chrono::seconds{30};
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::erase_if(impl_->transient_presence,
                  [&](const auto& value) { return value.at + lifetime <= now; });
    co_return impl_->transient_presence;
}

runtime::task<epoch> repository::compact(epoch_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto heads = co_await impl_->capture_document_heads();
    if (!heads) {
        co_return std::unexpected(heads.error());
    }
    auto compacted_bytes = std::uint64_t{};
    for (auto& [node, document] : impl_->directory_cache) {
        static_cast<void>(node);
        auto saved = document.save();
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        compacted_bytes += saved->size();
        auto reopened =
            crdt::directory_document::load_sync(*saved, "local", impl_->root_path.generic_string());
        if (!reopened) {
            co_return std::unexpected(reopened.error());
        }
        document = std::move(*reopened);
    }
    auto value =
        epoch{.n = impl_->current.epochs.empty() ? 1 : impl_->current.epochs.back().n + 1,
              .heads = std::move(*heads),
              .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
              .document_bytes = compacted_bytes};
    if (options.min_ack_peers != 0) {
        if (!impl_->acknowledge_epoch) {
            co_return std::unexpected(failure(errc::busy,
                                              "epoch acknowledgement source is not configured",
                                              std::to_string(options.min_ack_peers)));
        }
        auto acknowledged = co_await impl_->acknowledge_epoch(value);
        if (!acknowledged) {
            co_return std::unexpected(acknowledged.error());
        }
        if (*acknowledged < options.min_ack_peers) {
            co_return std::unexpected(failure(errc::busy,
                                              "peer acknowledgements are below min_ack_peers",
                                              std::to_string(*acknowledged)));
        }
    }
    impl_->current.epochs.push_back(value);
    impl_->current.drips.clear();
    impl_->current.operations.clear();
    impl_->current.pending_drips = 0;
    impl_->journal_bytes = 0;
    auto checkpointed = co_await impl_->checkpoint();
    if (!checkpointed) {
        co_return std::unexpected(checkpointed.error());
    }
    co_return value;
}

runtime::task<std::vector<epoch>> repository::epochs() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return impl_->current.epochs;
}

runtime::task<digest> repository::store_attestation(attestation value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::store_access::put_plain(impl_->objects,
                                                                  std::move(*encoded));
}

runtime::task<digest> repository::store_symbols(symbol_record value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto encoded = encode_symbols(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    const auto subject = to_string(value.subject);
    auto stored =
        co_await repository_detail::store_access::put_plain(impl_->objects, std::move(*encoded));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto text = to_string(*stored);
    impl_->current.metadata.insert_or_assign(
        "symbols/" + subject,
        std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                               reinterpret_cast<const std::byte*>(text.data() + text.size())});
    auto saved = co_await impl_->checkpoint();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return *stored;
}

runtime::task<symbol_record> repository::symbols(digest id) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto found = impl_->current.metadata.find("symbols/" + to_string(id));
    if (found == impl_->current.metadata.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "symbols are not recorded", to_string(id)));
    }
    const auto text =
        std::string_view{reinterpret_cast<const char*>(found->second.data()), found->second.size()};
    auto record = parse_digest(text);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto encoded = co_await impl_->objects.get(*record);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return decode_symbols(*encoded);
}

runtime::task<edit_result> repository::apply_edit(const edit_preview& preview) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto encoded_preview = encode_edit_preview(preview);
    if (!encoded_preview) {
        co_return std::unexpected(encoded_preview.error());
    }
    if (preview.changes.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "edit preview contains no accepted changes"));
    }
    auto prepared = std::vector<std::pair<std::string, std::vector<std::byte>>>{};
    prepared.reserve(preview.changes.size());
    for (const auto& file : preview.changes) {
        auto before = co_await read(file.path);
        if (!before && before.error().code != errc::not_found) {
            co_return std::unexpected(before.error());
        }
        auto current = before ? std::move(*before) : std::vector<std::byte>{};
        auto current_digest = hash(algorithm::blake3, current);
        if (!current_digest) {
            co_return std::unexpected(current_digest.error());
        }
        if (!same_digest(*current_digest, file.before)) {
            co_return std::unexpected(
                failure(errc::conflict, "structural edit preview is stale", file.path));
        }
        auto after = edit_detail::apply_changes(file, current);
        if (!after) {
            co_return std::unexpected(after.error());
        }
        prepared.emplace_back(file.path, std::move(*after));
    }
    const auto operation_start = impl_->current.operations.size();
    const auto drip_start = impl_->current.drips.size();
    for (auto& [path, contents] : prepared) {
        auto written = co_await write(path, std::move(contents));
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    auto grouped = stored_operation{};
    auto operation_id = random_operation_id(impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    grouped.id = *operation_id;
    grouped.kind = "structural_edit";
    grouped.from = to_string(preview.id);
    grouped.to = preview.attribution.tool + ":" + preview.attribution.operation;
    if (preview.attribution.agent) {
        grouped.to += ":" + *preview.attribution.agent;
    }
    for (auto index = operation_start; index < impl_->current.operations.size(); ++index) {
        auto& operation = impl_->current.operations[index];
        grouped.entries.insert(grouped.entries.end(),
                               std::make_move_iterator(operation.entries.begin()),
                               std::make_move_iterator(operation.entries.end()));
    }
    impl_->current.operations.erase(impl_->current.operations.begin() +
                                        static_cast<std::ptrdiff_t>(operation_start),
                                    impl_->current.operations.end());
    impl_->record_operation(std::move(grouped));
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    auto result_value = edit_result{.operation = *operation_id};
    result_value.drips.reserve(impl_->current.drips.size() - drip_start);
    for (auto index = drip_start; index < impl_->current.drips.size(); ++index) {
        auto id = hash(impl_->hash_algorithm, impl_->current.drips[index].body);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        result_value.drips.push_back({*id});
    }
    for (const auto& file : preview.changes) {
        auto after = co_await read(file.path);
        if (!after) {
            co_return std::unexpected(after.error());
        }
        auto actual = hash(algorithm::blake3, *after);
        if (!actual) {
            co_return std::unexpected(actual.error());
        }
        if (!same_digest(*actual, file.after)) {
            co_return std::unexpected(
                failure(errc::internal, "applied edit differs from its preview", file.path));
        }
    }
    co_return result_value;
}

runtime::task<void> repository::set_policy(policy_config policy) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto valid = build_detail::validate_policy(policy);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    impl_->current.policy = std::move(policy);
    co_return co_await impl_->checkpoint();
}

runtime::task<collection_report> repository::collect(collection_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto checkpointed = co_await impl_->checkpoint();
    if (!checkpointed) {
        co_return std::unexpected(checkpointed.error());
    }
    auto cache = build_detail::disk_cache{*this};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto retained = co_await cache.apply_retention(true);
    if (!retained) {
        co_return std::unexpected(retained.error());
    }
    co_return co_await impl_->objects.collect(options);
}

runtime::task<retention_explanation> repository::retain_explain(digest id) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto checkpointed = co_await impl_->checkpoint();
    if (!checkpointed) {
        co_return std::unexpected(checkpointed.error());
    }
    auto cache = build_detail::disk_cache{*this};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto retained = co_await cache.apply_retention(true);
    if (!retained) {
        co_return std::unexpected(retained.error());
    }
    auto options = collection_options{};
    options.dry_run = true;
    options.explain = true;
    auto report = co_await impl_->objects.collect(options);
    if (!report) {
        co_return std::unexpected(report.error());
    }
    const auto same_id = [&](const retained_chunk& item) {
        return item.id.alg == id.alg && item.id.bytes == id.bytes;
    };
    if (const auto found = std::ranges::find_if(report->retained_objects, same_id);
        found != report->retained_objects.end()) {
        co_return retention_explanation{id, true, found->reason};
    }
    if (const auto found = std::ranges::find_if(report->retained_chunks, same_id);
        found != report->retained_chunks.end()) {
        co_return retention_explanation{id, true, found->reason};
    }
    auto present = co_await impl_->objects.contains(id);
    if (!present) {
        co_return std::unexpected(present.error());
    }
    co_return retention_explanation{
        id,
        false,
        *present ? "not reachable from a live lease; the next collection will evict it"
                 : "not present in the local store"};
}

runtime::task<cache_report> repository::cache_stats() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return impl_->cache_statistics;
}

result<build_handle> repository::build(action_input input) {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return liquid::build::start(*this, std::move(input));
}

result<build_handle> repository::build(action_input input, remote_executor& remote) {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return liquid::build::start(*this, std::move(input), remote);
}

runtime::task<sync_export> repository::export_drips(journal_cursor cursor,
                                                    std::span<sync_item> output) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (output.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "sync export span must not be empty"));
    }
    auto position = parse_sync_cursor(cursor.token);
    if (!position) {
        co_return std::unexpected(position.error());
    }
    if (position->drips > impl_->current.drips.size()) {
        if (impl_->current.epochs.empty()) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "sync cursor is beyond the journal", cursor.token));
        }
        *position = {};
    }

    auto items = std::vector<sync_item>{};
    for (std::size_t index = position->drips; index < impl_->current.drips.size(); ++index) {
        const auto& current = impl_->current.drips[index];
        const auto author =
            current.author.taap_id.empty() ? impl_->signing_key.identity() : current.author;
        items.push_back({.kind = sync_item_kind::drip,
                         .author = author,
                         .body = current.body,
                         .signature = current.signature});
    }

    auto changes = std::deque<digest>{};
    for (const auto& [name, id] : impl_->current.refs) {
        static_cast<void>(name);
        changes.push_back(id.value);
    }
    auto visited_changes = std::set<std::string>{};
    auto object_roots = std::vector<digest>{};
    while (!changes.empty()) {
        const auto id = changes.front();
        changes.pop_front();
        if (!visited_changes.insert(to_string(id)).second) {
            continue;
        }
        auto encoded = co_await impl_->objects.get(id);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto value = objects::decode_change(*encoded, impl_->hash_algorithm);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto author = impl_->signing_key.identity();
        if (author.taap_id != value->author.taap_id) {
            const auto found = std::ranges::find_if(impl_->current.drips, [&](const auto& drip) {
                return drip.author.taap_id == value->author.taap_id;
            });
            if (found == impl_->current.drips.end()) {
                co_return std::unexpected(
                    failure(errc::corrupt, "change author identity is unavailable", to_string(id)));
            }
            author = found->author;
        }
        items.push_back({.kind = sync_item_kind::change,
                         .author = std::move(author),
                         .reference = change_id{id},
                         .body = std::move(*encoded),
                         .signature = value->sig});
        object_roots.push_back(id);
        for (const auto& parent : value->parents) {
            changes.push_back(parent.id.value);
        }
    }

    // Each author's identity chain travels ahead of the records it signs, so the receiver can
    // bind the author's name to its key (liquid#383). An author whose chain this machine does not
    // hold is sent without one, and the receiver refuses that author's records.
    auto authors = std::vector<actor>{};
    for (const auto& item : items) {
        if (!std::ranges::contains(authors, item.author.taap_id, &actor::taap_id)) {
            authors.push_back(item.author);
        }
    }
    auto identities = std::vector<sync_item>{};
    for (const auto& author : authors) {
        auto chain = load_identity_chain(author.taap_id);
        if (!chain && !is_unknown_signer(chain.error())) {
            co_return std::unexpected(chain.error());
        }
        const auto recorded =
            impl_->current.metadata.find(identity_chain_metadata_key(author.taap_id));
        if (recorded != impl_->current.metadata.end()) {
            auto kept = decode_identity_chain(recorded->second);
            if (kept && (!chain || kept->size() > chain->size())) {
                chain = std::move(*kept);
            }
        }
        if (!chain) {
            continue;
        }
        auto body = encode_identity_chain(*chain);
        if (!body) {
            co_return std::unexpected(body.error());
        }
        identities.push_back(
            {.kind = sync_item_kind::identity, .author = author, .body = std::move(*body)});
    }
    items.insert(items.begin(),
                 std::make_move_iterator(identities.begin()),
                 std::make_move_iterator(identities.end()));

    auto directories = std::set<node_id>{};
    append_working_directories(impl_->current.root, directories);
    for (const auto& node : directories) {
        auto document = co_await impl_->directory(node);
        if (!document) {
            co_return std::unexpected(document.error());
        }
        auto* working = directory_with_node(impl_->current.root, node);
        auto entries = (*document)->entries();
        if (!working || !entries) {
            co_return std::unexpected(
                !entries ? entries.error()
                         : failure(errc::corrupt, "directory disappeared during sync export"));
        }
        for (const auto& [name, child] : working->entries) {
            const auto found =
                std::ranges::find(*entries, name, &crdt::directory_document::named_entry::name);
            const auto expected = directory_entry(child);
            if (found == entries->end()) {
                co_return std::unexpected(
                    failure(errc::corrupt, "directory document misses a sync export entry", name));
            }
            if (found->value != expected) {
                const auto context = name + "; actual=" + found->value.kind + ":" +
                                     node_text(found->value.id) + ":" + found->value.ref +
                                     "; expected=" + expected.kind + ":" + node_text(expected.id) +
                                     ":" + expected.ref;
                co_return std::unexpected(failure(
                    errc::corrupt, "directory document differs during sync export", context));
            }
        }
        crdt::repo_id repository{};
        std::ranges::copy(impl_->current.repository, repository.begin());
        auto messages =
            export_directory_messages(**document, repository, impl_->root_path.generic_string());
        if (!messages) {
            co_return std::unexpected(messages.error());
        }
        for (auto& message : *messages) {
            items.push_back({.kind = sync_item_kind::directory,
                             .repository = impl_->current.repository,
                             .directory = node,
                             .root = node == impl_->current.root.node,
                             .body = std::move(message)});
        }
    }
    auto active_presence = co_await presence();
    if (!active_presence) {
        co_return std::unexpected(active_presence.error());
    }
    for (const auto& value : *active_presence) {
        auto body = encode_presence_body(value);
        if (!body) {
            co_return std::unexpected(body.error());
        }
        items.push_back({.kind = sync_item_kind::presence, .body = std::move(*body)});
    }
    for (const auto& [name, id] : impl_->current.refs) {
        items.push_back({.kind = sync_item_kind::reference, .name = name, .reference = id});
    }
    append_working_roots(impl_->current.root, object_roots);
    for (const auto& conflict : impl_->current.conflict_values) {
        if (conflict.id.alg != algorithm{}) {
            object_roots.push_back(conflict.id);
        }
        object_roots.insert(
            object_roots.end(), conflict.legacy_refs.begin(), conflict.legacy_refs.end());
    }
    for (const auto& [node, id] : impl_->current.dirs) {
        static_cast<void>(node);
        object_roots.push_back(id);
    }
    std::ranges::sort(object_roots, {}, [](const digest& id) { return to_string(id); });
    object_roots.erase(std::unique(object_roots.begin(), object_roots.end(), same_digest),
                       object_roots.end());

    if (position->items > items.size()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "sync cursor item offset is invalid", cursor.token));
    }
    const auto available = items.size() - position->items;
    const auto count = std::min(output.size(), available);
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = std::move(items[position->items + index]);
    }
    const auto done = position->items + count == items.size();
    auto next = journal_cursor{};
    if (done) {
        next.token = std::to_string(impl_->current.drips.size());
    } else {
        next.token =
            std::to_string(position->drips) + ":" + std::to_string(position->items + count);
    }
    co_return sync_export{std::move(next), count, done, std::move(object_roots)};
}

runtime::task<sync_import_report> repository::import_drips(std::span<const sync_item> input) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto reported_items = input.size();
    auto durable = std::vector<sync_item>{};
    durable.reserve(input.size());
    for (const auto& item : input) {
        if (item.kind != sync_item_kind::presence) {
            durable.push_back(item);
            continue;
        }
        auto value = decode_presence_body(item.body);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto reported = co_await report_presence(std::move(*value));
        if (!reported) {
            co_return std::unexpected(reported.error());
        }
    }
    input = std::span<const sync_item>{durable};
    if (input.empty()) {
        co_return sync_import_report{reported_items};
    }
    auto candidate_bytes = encode_repository_document(impl_->current);
    if (!candidate_bytes) {
        co_return std::unexpected(candidate_bytes.error());
    }
    auto candidate = decode_repository_document(*candidate_bytes);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }

    const auto root_item = std::ranges::find_if(input, [](const sync_item& item) {
        return item.kind == sync_item_kind::directory && item.root;
    });
    auto identity_adopted = false;
    if (root_item != input.end() && root_item->directory != candidate->root.node) {
        const auto empty =
            candidate->refs.empty() && candidate->drips.empty() && candidate->root.entries.empty();
        if (!empty) {
            co_return std::unexpected(
                failure(errc::conflict, "cannot replace the identity of a non-empty repository"));
        }
        candidate->repository = root_item->repository;
        candidate->root.node = root_item->directory;
        candidate->dirs.clear();
        identity_adopted = true;
    }

    auto candidate_nodes = std::set<node_id>{};
    if (!identity_adopted) {
        append_working_directories(impl_->current.root, candidate_nodes);
    }
    auto candidate_documents = std::map<node_id, crdt::directory_document>{};
    for (const auto& node : candidate_nodes) {
        auto current = co_await impl_->directory(node);
        if (!current) {
            co_return std::unexpected(current.error());
        }
        auto saved = (*current)->save();
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        auto copied =
            crdt::directory_document::load_sync(*saved, "local", impl_->root_path.generic_string());
        if (!copied) {
            saved = (*current)->save_full();
            if (!saved) {
                co_return std::unexpected(saved.error());
            }
            copied = crdt::directory_document::load_sync(
                *saved, "local", impl_->root_path.generic_string());
            if (!copied) {
                auto problem = copied.error();
                problem.context = "copy sync candidate: " + problem.context;
                co_return std::unexpected(std::move(problem));
            }
        }
        candidate_documents.emplace(node, std::move(*copied));
    }

    auto received_documents = std::map<node_id, crdt::directory_document>{};
    auto directory_states = std::map<node_id, std::unique_ptr<crdt::sync_state>>{};
    auto imported_drips = std::vector<persisted_drip>{};
    auto imported_refs = std::vector<std::pair<std::string, change_id>>{};
    auto remote_writes = std::map<node_id, digest>{};
    repository_detail::store_access::begin_object_batch(impl_->objects);
    auto abort = [&] { repository_detail::store_access::abort_object_batch(impl_->objects); };
    auto import_state = sync_import_state{*candidate,
                                          impl_->objects,
                                          impl_->hash_algorithm,
                                          received_documents,
                                          directory_states,
                                          imported_drips,
                                          imported_refs,
                                          impl_->root_path.generic_string()};
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto imported = co_await import_sync_item(import_state, input[index]);
        if (!imported) {
            abort();
            co_return std::unexpected(sync_item_failure(index, imported.error()));
        }
    }

    for (auto& [node, received] : received_documents) {
        const auto candidate = candidate_documents.find(node);
        if (candidate == candidate_documents.end()) {
            candidate_documents.emplace(node, std::move(received));
            continue;
        }
        auto local_operations = candidate->second.operations();
        auto remote_operations = received.operations();
        if (!local_operations || !remote_operations) {
            abort();
            co_return std::unexpected(!local_operations ? local_operations.error()
                                                        : remote_operations.error());
        }
        auto local_ids = std::set<node_id>{};
        for (const auto& operation : *local_operations) {
            local_ids.insert(operation.id);
        }
        for (const auto& operation : *remote_operations) {
            if (operation.kind != crdt::operation_kind::write || local_ids.contains(operation.id)) {
                continue;
            }
            auto reference = parse_digest(operation.ref);
            if (!reference) {
                abort();
                co_return std::unexpected(reference.error());
            }
            remote_writes[operation.entry_id] = *reference;
        }
        auto merged = candidate->second.merge(received);
        if (!merged) {
            abort();
            co_return std::unexpected(merged.error());
        }
    }

    auto directories_applied = apply_synced_directories(*candidate, candidate_documents);
    if (!directories_applied) {
        abort();
        co_return std::unexpected(directories_applied.error());
    }
    auto refs_applied = co_await apply_synced_refs(impl_->objects, *candidate, imported_refs);
    if (!refs_applied) {
        abort();
        co_return std::unexpected(refs_applied.error());
    }
    auto remote_head = std::optional<change_id>{};
    for (const auto& [name, reference] : imported_refs) {
        if (name == "main") {
            remote_head = reference;
        }
    }
    auto merge_metadata = std::map<std::string, std::string>{};
    auto graph = std::optional<frontend_graph>{};
#if defined(LIQUID_HAS_BUILTIN_FRONTEND)
    // The parse graph is a local cache, not repository state. A state backend without a local
    // root (liquid #326) gets a scratch directory for the length of this merge instead.
    auto scratch = scratch_directory{};
    if (!remote_writes.empty()) {
        auto linked = frontend_module::linked(liquid_builtin_tree_sitter_module_v1());
        if (!linked) {
            abort();
            co_return std::unexpected(linked.error());
        }
        auto module = std::make_shared<frontend_module>(std::move(*linked));
        auto graph_root = store_detail::backend_local_path(
            impl_->state.backend, impl_->state.key("STRUCTURAL_MERGE_GRAPH"));
        if (!graph_root) {
            auto created = scratch.create("liquid-merge-graph-");
            if (!created) {
                abort();
                co_return std::unexpected(created.error());
            }
            graph_root = *created;
        }
        auto opened = co_await frontend_graph::open(
            *graph_root,
            graph_root->parent_path(),
            std::move(module),
            [](std::vector<std::byte>) -> runtime::task<std::vector<std::byte>> {
                co_return std::unexpected(failure(
                    errc::unsupported, "structural merge does not execute compiler actions"));
            });
        if (!opened) {
            abort();
            co_return std::unexpected(opened.error());
        }
        graph = std::move(*opened);
    }
#endif
    auto structurally_merged = co_await apply_structural_merges(impl_->objects,
                                                                impl_->current,
                                                                *candidate,
                                                                candidate_documents,
                                                                remote_writes,
                                                                remote_head,
                                                                graph ? &*graph : nullptr,
                                                                merge_metadata);
    if (!structurally_merged) {
        abort();
        co_return std::unexpected(structurally_merged.error());
    }
    auto locks_applied = co_await reconcile_lock_drips(*candidate, imported_drips, impl_->objects);
    if (!locks_applied) {
        abort();
        co_return std::unexpected(locks_applied.error());
    }
    candidate->drips.insert(candidate->drips.end(), imported_drips.begin(), imported_drips.end());
    candidate->pending_drips += imported_drips.size();
    auto merge = merge_operation(impl_->current,
                                 *candidate,
                                 impl_->hash_algorithm,
                                 impl_->session,
                                 impl_->signing_key.identity(),
                                 input.size(),
                                 std::move(merge_metadata));
    if (!merge) {
        abort();
        co_return std::unexpected(merge.error());
    }
    candidate->operations.push_back(std::move(*merge));
    auto committed = co_await repository_detail::store_access::commit_object_batch(impl_->objects);
    if (!committed) {
        abort();
        co_return std::unexpected(committed.error());
    }

    auto previous = std::move(impl_->current);
    auto previous_documents = std::move(impl_->directory_cache);
    impl_->current = std::move(*candidate);
    impl_->directory_cache = std::move(candidate_documents);
    auto checkpointed = co_await impl_->checkpoint();
    if (!checkpointed) {
        impl_->current = std::move(previous);
        impl_->directory_cache = std::move(previous_documents);
        co_return std::unexpected(checkpointed.error());
    }
    co_return sync_import_report{reported_items};
}

namespace {

runtime::task<bool> remotely_available(store& objects, const blob& record) {
    if (record.remote) {
        co_return true;
    }
    for (const auto& chunk : record.chunks) {
        auto availability = co_await objects.has(chunk);
        if (!availability) {
            co_return std::unexpected(availability.error());
        }
        if (*availability == blob_availability::remote) {
            co_return true;
        }
    }
    co_return false;
}

} // namespace

runtime::task<repository_detail::vfs_entry> repository_detail::vfs_access::entry(repository& source,
                                                                                 std::string path) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    working_entry* found = &source.impl_->current.root;
    std::string name;
    if (!path.empty()) {
        auto parts = split_path(path);
        if (!parts)
            co_return std::unexpected(parts.error());
        found = lookup(source.impl_->current.root, *parts);
        name = parts->back();
    }
    if (!found)
        co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
    vfs_entry output{.name = std::move(name),
                     .kind = found->kind == entry_kind::file ? node_kind::blob : node_kind::tree,
                     .mode = found->mode,
                     .node = found->node,
                     .object = found->object};
    if (found->kind == entry_kind::nested_repository)
        output.kind = node_kind::repo;
    if (found->kind == entry_kind::file) {
        auto encoded = co_await source.impl_->objects.get(found->object);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        auto record = objects::decode_blob(*encoded);
        if (!record)
            co_return std::unexpected(record.error());
        auto remote = co_await remotely_available(source.impl_->objects, *record);
        if (!remote)
            co_return std::unexpected(remote.error());
        output.size = record->size;
        output.chunks = std::move(record->chunks);
        output.chunk_sizes = std::move(record->chunk_sizes);
        output.content = record->content;
        output.remote = std::move(record->remote);
        output.remotely_available = *remote;
    }
    co_return output;
}

namespace {

runtime::task<std::optional<tree_entry>>
snapshot_entry(store& objects, digest root, std::string_view path) {
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    for (auto index = std::size_t{}; index < parts->size(); ++index) {
        const auto& part = (*parts)[index];
        auto encoded = co_await objects.get(root);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto tree = objects::decode_tree(*encoded);
        if (!tree) {
            co_return std::unexpected(tree.error());
        }
        const auto found = std::ranges::find(tree->entries, part, &tree_entry::name);
        if (found == tree->entries.end()) {
            co_return std::optional<tree_entry>{};
        }
        if (index + 1 == parts->size()) {
            co_return *found;
        }
        if (found->kind != node_kind::tree && found->kind != node_kind::repo) {
            co_return std::optional<tree_entry>{};
        }
        root = found->ref;
    }
    co_return std::optional<tree_entry>{};
}

runtime::task<repository_detail::vfs_entry> snapshot_vfs_entry(store& objects,
                                                               const tree_entry& entry) {
    auto output = repository_detail::vfs_entry{.name = entry.name,
                                               .kind = entry.kind,
                                               .mode = entry.mode,
                                               .node = entry.node,
                                               .object = entry.ref};
    if (entry.kind != node_kind::blob) {
        co_return output;
    }
    auto encoded = co_await objects.get(entry.ref);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = objects::decode_blob(*encoded);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto remote = co_await remotely_available(objects, *record);
    if (!remote) {
        co_return std::unexpected(remote.error());
    }
    output.size = record->size;
    output.chunks = std::move(record->chunks);
    output.chunk_sizes = std::move(record->chunk_sizes);
    output.content = record->content;
    output.remote = std::move(record->remote);
    output.remotely_available = *remote;
    co_return output;
}

runtime::task<tree> snapshot_tree(store& objects, digest root, std::string path) {
    if (!path.empty()) {
        auto entry = co_await snapshot_entry(objects, root, path);
        if (!entry) {
            co_return std::unexpected(entry.error());
        }
        if (!*entry) {
            co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
        }
        if ((*entry)->kind != node_kind::tree && (*entry)->kind != node_kind::repo) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "path is not a directory", path));
        }
        root = (*entry)->ref;
    }
    auto encoded = co_await objects.get(root);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return objects::decode_tree(*encoded);
}

struct located_snapshot_entry {
    std::string path;
    tree_entry entry;
};

runtime::task<std::optional<located_snapshot_entry>>
snapshot_node(store& objects, digest root, const node_id& node, std::string prefix = {}) {
    auto encoded = co_await objects.get(root);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto tree = objects::decode_tree(*encoded);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    for (const auto& entry : tree->entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.node == node) {
            co_return located_snapshot_entry{path, entry};
        }
        if (entry.kind != node_kind::tree) {
            continue;
        }
        auto nested = co_await snapshot_node(objects, entry.ref, node, path);
        if (!nested) {
            co_return std::unexpected(nested.error());
        }
        if (*nested) {
            co_return std::move(**nested);
        }
    }
    co_return std::optional<located_snapshot_entry>{};
}

} // namespace

runtime::task<std::vector<path_history_entry>> repository::history(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto changes = co_await log();
    if (!changes) {
        co_return std::unexpected(changes.error());
    }
    auto tracked = std::optional<node_id>{};
    for (const auto& change : *changes) {
        auto found = co_await snapshot_entry(impl_->objects, change.root, path);
        if (!found) {
            co_return std::unexpected(found.error());
        }
        if (*found) {
            if ((*found)->kind != node_kind::blob && (*found)->kind != node_kind::conflict) {
                co_return std::unexpected(
                    failure(errc::invalid_argument, "history path is not a file", path));
            }
            tracked = (*found)->node;
            break;
        }
    }
    if (!tracked) {
        co_return std::unexpected(failure(errc::not_found, "history path does not exist", path));
    }
    auto states = std::vector<std::optional<located_snapshot_entry>>{};
    states.reserve(changes->size());
    for (const auto& change : *changes) {
        auto found = co_await snapshot_node(impl_->objects, change.root, *tracked);
        if (!found) {
            co_return std::unexpected(found.error());
        }
        states.push_back(std::move(*found));
    }
    auto output = std::vector<path_history_entry>{};
    for (auto index = std::size_t{}; index < changes->size(); ++index) {
        const auto& current = states[index];
        const auto& older =
            index + 1 < states.size() ? states[index + 1] : std::optional<located_snapshot_entry>{};
        auto changed = current.has_value() != older.has_value();
        if (current && older) {
            changed = current->path != older->path || current->entry.mode != older->entry.mode ||
                      current->entry.ref.alg != older->entry.ref.alg ||
                      current->entry.ref.bytes != older->entry.ref.bytes;
        }
        if (!changed) {
            continue;
        }
        const auto& change = (*changes)[index];
        if (current) {
            output.push_back({.change = change.id,
                              .path = current->path,
                              .node = current->entry.node,
                              .value = current->entry.ref,
                              .mode = current->entry.mode,
                              .by = change.author,
                              .at = change.at,
                              .message = change.message,
                              .identity_verified = change.identity_verified});
            continue;
        }
        output.push_back({.change = change.id,
                          .path = older->path,
                          .node = older->entry.node,
                          .by = change.author,
                          .at = change.at,
                          .message = change.message,
                          .deleted = true,
                          .identity_verified = change.identity_verified});
    }
    co_return output;
}

runtime::task<void> repository::restore(std::string path, change_id selected) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto entries = co_await history(path);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    const auto found = std::ranges::find_if(*entries, [&](const path_history_entry& entry) {
        return entry.change.value.alg == selected.value.alg &&
               entry.change.value.bytes == selected.value.bytes;
    });
    if (found == entries->end()) {
        co_return std::unexpected(failure(
            errc::not_found, "change is not in the path history", to_string(selected.value)));
    }
    if (found->deleted || !found->value) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "cannot restore a deletion", found->path));
    }
    auto encoded = co_await impl_->objects.get(*found->value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto value = objects::decode_blob(*encoded);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto restored = co_await impl_->record_blob(std::move(path),
                                                std::move(*value),
                                                write_options{.mode = found->mode},
                                                std::nullopt,
                                                found->node);
    if (!restored) {
        co_return std::unexpected(restored.error());
    }
    co_return result<void>{};
}

runtime::task<repository_detail::vfs_entry>
repository_detail::vfs_access::entry(repository& source, digest root, std::string path) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (path.empty()) {
        co_return vfs_entry{.kind = node_kind::tree, .mode = 0040755, .object = root};
    }
    auto found = co_await snapshot_entry(source.impl_->objects, root, path);
    if (!found) {
        co_return std::unexpected(found.error());
    }
    if (!*found) {
        co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
    }
    co_return co_await snapshot_vfs_entry(source.impl_->objects, **found);
}

runtime::task<std::vector<repository_detail::vfs_entry>>
repository_detail::vfs_access::children(repository& source, std::string path) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    working_entry* directory = &source.impl_->current.root;
    if (!path.empty()) {
        auto parts = split_path(path);
        if (!parts)
            co_return std::unexpected(parts.error());
        directory = lookup(source.impl_->current.root, *parts);
    }
    if (!directory || directory->kind == entry_kind::file)
        co_return std::unexpected(failure(errc::not_found, "directory does not exist", path));
    auto output = std::vector<vfs_entry>{};
    output.reserve(directory->entries.size());
    for (const auto& [name, child] : directory->entries) {
        vfs_entry value{.name = name,
                        .kind = child.kind == entry_kind::file ? node_kind::blob : node_kind::tree,
                        .mode = child.mode,
                        .node = child.node,
                        .object = child.object};
        if (child.kind == entry_kind::nested_repository)
            value.kind = node_kind::repo;
        if (child.kind == entry_kind::file) {
            auto encoded = co_await source.impl_->objects.get(child.object);
            if (!encoded)
                co_return std::unexpected(encoded.error());
            auto record = objects::decode_blob(*encoded);
            if (!record)
                co_return std::unexpected(record.error());
            auto remote = co_await remotely_available(source.impl_->objects, *record);
            if (!remote)
                co_return std::unexpected(remote.error());
            value.size = record->size;
            value.chunks = std::move(record->chunks);
            value.chunk_sizes = std::move(record->chunk_sizes);
            value.content = record->content;
            value.remote = std::move(record->remote);
            value.remotely_available = *remote;
        }
        output.push_back(std::move(value));
    }
    co_return output;
}

runtime::task<std::vector<repository_detail::vfs_entry>>
repository_detail::vfs_access::children(repository& source, digest root, std::string path) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto directory = co_await snapshot_tree(source.impl_->objects, root, path);
    if (!directory) {
        co_return std::unexpected(directory.error());
    }
    auto output = std::vector<vfs_entry>{};
    output.reserve(directory->entries.size());
    for (const auto& entry : directory->entries) {
        auto value = co_await snapshot_vfs_entry(source.impl_->objects, entry);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        output.push_back(std::move(*value));
    }
    co_return output;
}

runtime::task<std::uint64_t> repository_detail::vfs_access::read_chunk(repository& source,
                                                                       digest id,
                                                                       std::uint64_t offset,
                                                                       std::span<std::byte> into) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await source.impl_->objects.read(id, offset, into);
}

runtime::task<void> repository_detail::vfs_access::make_directory(repository& source,
                                                                  std::string path,
                                                                  std::uint32_t mode) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto lock_allowed = source.impl_->validate_lock_write(path);
    if (!lock_allowed) {
        co_return std::unexpected(lock_allowed.error());
    }
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent =
        parent_for(source.impl_->current.root, *parts, false, source.impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    if ((*parent)->entries.contains(parts->back()))
        co_return std::unexpected(failure(errc::already_exists, "path already exists", path));
    auto directory = co_await source.impl_->directory((*parent)->node);
    if (!directory)
        co_return std::unexpected(directory.error());
    const auto identity = random_id();
    const auto reference = digest{source.impl_->hash_algorithm, {}};
    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto time =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    auto recorded = (*directory)
                        ->put_entry(parts->back(),
                                    {"tree", identity, mode, to_string(reference)},
                                    crdt::commit_options{.message = "mkdir", .time = time});
    if (!recorded)
        co_return std::unexpected(recorded.error());
    (*parent)->entries.emplace(parts->back(),
                               working_entry{entry_kind::directory, identity, mode, reference, {}});
    auto operation = random_operation_id(source.impl_->hash_algorithm);
    if (!operation)
        co_return std::unexpected(operation.error());
    source.impl_->record_operation(*operation, "mkdir", path, {}, identity, std::nullopt);
    auto signed_drip = co_await source.impl_->append_drip(
        "mkdir", identity, at, {path}, write_options{}, std::nullopt);
    if (!signed_drip)
        co_return std::unexpected(signed_drip.error());
    ++source.impl_->current.pending_drips;
    auto saved = co_await source.impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}

runtime::task<void>
repository_detail::vfs_access::set_mode(repository& source, std::string path, std::uint32_t mode) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent =
        parent_for(source.impl_->current.root, *parts, false, source.impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    auto found = (*parent)->entries.find(parts->back());
    if (found == (*parent)->entries.end())
        co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
    const auto before = found->second;
    auto directory = co_await source.impl_->directory((*parent)->node);
    if (!directory)
        co_return std::unexpected(directory.error());
    const auto kind = found->second.kind == entry_kind::file        ? "blob"
                      : found->second.kind == entry_kind::directory ? "tree"
                                                                    : "repo";
    auto recorded =
        (*directory)
            ->put_entry(parts->back(),
                        {kind, found->second.node, mode, to_string(found->second.object)});
    if (!recorded)
        co_return std::unexpected(recorded.error());
    found->second.mode = mode;
    auto operation_id = random_operation_id(source.impl_->hash_algorithm);
    if (!operation_id) {
        co_return std::unexpected(operation_id.error());
    }
    auto operation = stored_operation{};
    operation.id = *operation_id;
    operation.kind = "set_mode";
    operation.from = path;
    operation.entries.push_back({path, before, found->second});
    source.impl_->record_operation(std::move(operation));
    auto saved = co_await source.impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}

result<std::filesystem::path> repository_detail::vfs_access::root_path(repository& source) {
    if (!source.impl_)
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    return source.impl_->root_path;
}

runtime::task<void> repository_detail::vfs_access::register_remote_source(
    repository& source, std::string id, std::shared_ptr<remote_blob_source> remote) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await source.impl_->objects.register_remote_source(std::move(id),
                                                                    std::move(remote));
}

runtime::task<std::vector<std::byte>> repository_detail::build_access::get(repository& source,
                                                                           digest id) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await source.impl_->objects.get(id);
}

runtime::task<digest> repository_detail::build_access::put_record(repository& source,
                                                                  std::vector<std::byte> bytes) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await repository_detail::store_access::put_plain(source.impl_->objects,
                                                                  std::move(bytes));
}

runtime::task<digest> repository_detail::build_access::put_record(repository& source,
                                                                  std::vector<std::byte> bytes,
                                                                  algorithm hash_algorithm) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await repository_detail::store_access::put_plain(
        source.impl_->objects, std::move(bytes), hash_algorithm);
}

runtime::task<digest> repository_detail::build_access::put_file(repository& source,
                                                                std::filesystem::path path) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto content = co_await source.impl_->objects.put_file(std::move(path));
    if (!content)
        co_return std::unexpected(content.error());
    auto blob = repository_detail::store_access::blob_for(source.impl_->objects, *content);
    if (!blob)
        co_return std::unexpected(blob.error());
    blob->content = {};
    auto encoded = objects::encode(*blob);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    co_return co_await repository_detail::store_access::put_plain(source.impl_->objects,
                                                                  std::move(*encoded));
}

runtime::task<std::uint64_t> repository_detail::build_access::read(repository& source,
                                                                   digest id,
                                                                   std::uint64_t offset,
                                                                   std::span<std::byte> output) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await source.impl_->objects.read(id, offset, output);
}

runtime::task<std::chrono::sys_seconds>
repository_detail::build_access::change_time(repository& source, digest tree) {
    if (!source.impl_ || !source.impl_->current.head)
        co_return std::unexpected(
            failure(errc::not_found, "repository has no change for action inputs"));
    auto bytes = co_await source.impl_->objects.get(source.impl_->current.head->value);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    auto change = objects::decode_change(*bytes);
    if (!change)
        co_return std::unexpected(change.error());
    if (to_string(change->root) != to_string(tree))
        co_return std::unexpected(failure(errc::invalid_argument,
                                          "action inputs are not the current change tree",
                                          to_string(tree)));
    co_return change->at;
}

runtime::task<std::optional<change_attribution>>
repository_detail::build_access::path_change(repository& source, std::string path, digest content) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto parts = split_path(path);
    if (!parts) {
        co_return std::unexpected(parts.error());
    }
    const auto* current = lookup(source.impl_->current.root, *parts);
    if (!current || current->kind != entry_kind::file || current->object.alg != content.alg ||
        current->object.bytes != content.bytes) {
        co_return std::nullopt;
    }
    const auto operation = std::ranges::find_if(
        source.impl_->current.operations.rbegin(),
        source.impl_->current.operations.rend(),
        [&](const stored_operation& value) { return value.kind == "write" && value.from == path; });
    if (operation == source.impl_->current.operations.rend() || operation->node != current->node) {
        co_return std::nullopt;
    }
    for (auto candidate = source.impl_->current.drips.rbegin();
         candidate != source.impl_->current.drips.rend();
         ++candidate) {
        auto decoded = cbor::decode(candidate->body);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto* kind_value = find(*decoded, "kind");
        const auto* operation_value = find(*decoded, "operation");
        const auto* author_value = find(*decoded, "author");
        const auto* at_value = find(*decoded, "at");
        const auto* node_value = find(*decoded, "node");
        const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
        const auto* operation_name =
            operation_value ? std::get_if<std::string>(&operation_value->data) : nullptr;
        const auto* author = author_value ? std::get_if<std::string>(&author_value->data) : nullptr;
        auto at = std::optional<std::int64_t>{};
        if (const auto* signed_at =
                at_value ? std::get_if<std::int64_t>(&at_value->data) : nullptr) {
            at = *signed_at;
        } else if (const auto* unsigned_at =
                       at_value ? std::get_if<std::uint64_t>(&at_value->data) : nullptr;
                   unsigned_at && *unsigned_at <= static_cast<std::uint64_t>(
                                                      std::numeric_limits<std::int64_t>::max())) {
            at = static_cast<std::int64_t>(*unsigned_at);
        }
        const auto* node =
            node_value ? std::get_if<cbor::value::bytes>(&node_value->data) : nullptr;
        if (!kind || *kind != "drip" || !operation_name || *operation_name != "write" || !author ||
            !at || !node || node->size() != operation->node.size()) {
            continue;
        }
        if (*author != operation->by.taap_id ||
            std::chrono::sys_seconds{std::chrono::seconds{*at}} != operation->at ||
            !std::ranges::equal(*node, operation->node)) {
            continue;
        }
        auto id = hash(source.impl_->hash_algorithm, candidate->body);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        // The write is shown as its author's only when the drip that records it checks against
        // that author's identity chain (liquid#508).
        co_return change_attribution{.drip = *id,
                                     .by = operation->by,
                                     .at = operation->at,
                                     .identity_verified = drip_signed_by_verified_identity(
                                         *candidate, source.impl_->signing_key.identity())};
    }
    co_return std::nullopt;
}

result<std::filesystem::path> repository_detail::build_access::root_path(repository& source) {
    if (!source.impl_)
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    return source.impl_->root_path;
}

runtime::task<bool> repository_detail::build_access::contains(repository& source, digest id) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await source.impl_->objects.contains(id);
}

std::vector<digest> repository_detail::build_access::attestations(repository& source,
                                                                  digest subject) {
    if (!source.impl_) {
        return {};
    }
    return repository_detail::store_access::attestations(source.impl_->objects, subject);
}

runtime::task<void> repository_detail::build_access::sign_result(repository& source,
                                                                 action_result& value) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    value.producer = source.impl_->signing_key.identity();
    auto body = objects::encode_result_body(value);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await source.impl_->signing_key.sign("result", std::move(*body));
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto signature = identity_detail::canonical_signature(*signed_value);
    if (!signature) {
        co_return std::unexpected(signature.error());
    }
    value.sig = std::move(*signature);
    co_return result<void>{};
}

runtime::task<void> repository_detail::build_access::sign_attestation(repository& source,
                                                                      attestation& value) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    value.issuer = source.impl_->signing_key.identity();
    auto body = objects::encode_attestation_body(value);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await source.impl_->signing_key.sign("attestation", std::move(*body));
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto signature = identity_detail::canonical_signature(*signed_value);
    if (!signature) {
        co_return std::unexpected(signature.error());
    }
    value.signature = std::move(*signature);
    co_return result<void>{};
}

auto repository_detail::build_access::signer(repository& source) -> result<actor> {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->signing_key.identity();
}

runtime::task<void> repository_detail::build_access::retain_cache_root(repository& source,
                                                                       digest root) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await source.impl_->retain(source.impl_->cache_root_lease, root);
}

runtime::task<void> repository_detail::build_access::release_cache_root(repository& source) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (source.impl_->cache_root_lease) {
        auto released = co_await source.impl_->cache_root_lease->value.release();
        if (!released) {
            co_return std::unexpected(released.error());
        }
        source.impl_->cache_root_lease.reset();
    }
    co_return co_await source.impl_->sync_retention_roots("retention:result\n", {});
}

bool repository_detail::build_access::cache_publish_enabled(repository& source) noexcept {
    return source.impl_ && source.impl_->current.cache_publish;
}

std::uint32_t repository_detail::build_access::verification_rate(repository& source) noexcept {
    return source.impl_ ? source.impl_->current.verification_rate : 20;
}

result<policy_config> repository_detail::build_access::policy(repository& source) {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->current.policy;
}

retention_config repository_detail::build_access::retention_policy(repository& source) {
    return source.impl_ ? source.impl_->current.retention : retention_config{};
}

runtime::task<void> repository_detail::build_access::sync_retention_roots(
    repository& source, std::string owner_prefix, std::vector<retention_root> roots) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await source.impl_->sync_retention_roots(std::move(owner_prefix),
                                                          std::move(roots));
}

void repository_detail::build_access::record_cache_lookup(repository& source,
                                                          digest result,
                                                          cache_status status,
                                                          std::optional<cache_miss> why,
                                                          std::uint64_t bytes_served) {
    if (!source.impl_) {
        return;
    }
    source.impl_->cache_lookups.insert_or_assign(to_string(result),
                                                 std::pair{status, std::move(why)});
    if (status == cache_status::hit) {
        ++source.impl_->cache_statistics.hits;
        source.impl_->cache_statistics.bytes_served += bytes_served;
    } else {
        ++source.impl_->cache_statistics.misses;
    }
}

void repository_detail::build_access::record_cache_store(repository& source, std::uint64_t bytes) {
    if (source.impl_) {
        source.impl_->cache_statistics.bytes_stored += bytes;
    }
}

void repository_detail::build_access::record_critical_alert(repository& source,
                                                            critical_cache_alert alert) {
    if (source.impl_) {
        source.impl_->cache_statistics.critical_alerts.push_back(std::move(alert));
    }
}

void repository_detail::build_access::apply_cache_lookup(repository& source,
                                                         digest result,
                                                         action_result& value) {
    if (!source.impl_) {
        return;
    }
    const auto found = source.impl_->cache_lookups.find(to_string(result));
    if (found == source.impl_->cache_lookups.end()) {
        return;
    }
    value.cache = found->second.first;
    value.why = found->second.second;
}

result<data_config> repository_detail::data_access::configuration(repository& source) {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->current.data;
}

result<std::vector<repository_detail::stored_data_branch>>
repository_detail::data_access::branches(repository& source) {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->current.data_branches;
}

result<std::filesystem::path> repository_detail::data_access::root_path(repository& source) {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->root_path;
}

runtime::task<void> repository_detail::data_access::add(repository& source,
                                                        stored_data_branch branch) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto duplicate =
        std::ranges::find_if(source.impl_->current.data_branches, [&](const auto& existing) {
            return existing.record.id == branch.record.id ||
                   (existing.record.session == branch.record.session &&
                    existing.record.database == branch.record.database);
        });
    if (duplicate != source.impl_->current.data_branches.end()) {
        co_return std::unexpected(failure(errc::already_exists,
                                          "data branch already exists for the session",
                                          branch.record.database));
    }
    source.impl_->current.data_branches.push_back(std::move(branch));
    auto saved = co_await source.impl_->checkpoint();
    if (!saved) {
        source.impl_->current.data_branches.pop_back();
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}

runtime::task<void> repository_detail::data_access::remove(repository& source, std::string id) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    const auto found = std::ranges::find(source.impl_->current.data_branches,
                                         id,
                                         [](const auto& branch) { return branch.record.id; });
    if (found == source.impl_->current.data_branches.end()) {
        co_return std::unexpected(failure(errc::not_found, "data branch is not recorded", id));
    }
    auto removed = *found;
    const auto offset =
        static_cast<std::size_t>(std::distance(source.impl_->current.data_branches.begin(), found));
    source.impl_->current.data_branches.erase(found);
    auto saved = co_await source.impl_->checkpoint();
    if (!saved) {
        source.impl_->current.data_branches.insert(source.impl_->current.data_branches.begin() +
                                                       static_cast<std::ptrdiff_t>(offset),
                                                   std::move(removed));
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}

auto repository_detail::testing::directory_loads(repository& value) -> std::uint64_t {
    return value.impl_ ? value.impl_->directory_load_count : 0;
}

auto repository_detail::testing::last_operation(repository& value) -> op_id {
    return value.impl_ && !value.impl_->current.operations.empty()
               ? value.impl_->current.operations.back().id
               : op_id{};
}

auto repository_detail::testing::last_operation_node(repository& value) -> node_id {
    return value.impl_ && !value.impl_->current.operations.empty()
               ? value.impl_->current.operations.back().node
               : node_id{};
}

runtime::task<repository> repository_detail::testing::clone(const repository& source,
                                                            std::filesystem::path destination) {
    if (!source.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    std::error_code code;
    std::filesystem::copy(
        source.impl_->root_path, destination, std::filesystem::copy_options::recursive, code);
    if (code)
        co_return std::unexpected(failure(errc::io, code.message(), destination.string()));
    repository_options options;
    options.checkpoint_interval = source.impl_->checkpoint_interval;
    options.storage.hash_algorithm = source.impl_->hash_algorithm;
    co_return co_await repository::open(std::move(destination), std::move(options));
}

runtime::task<void> repository_detail::testing::sync(repository& left, repository& right) {
    if (!left.impl_ || !right.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (left.impl_->current.repository != right.impl_->current.repository)
        co_return std::unexpected(failure(errc::invalid_argument, "repository identities differ"));

    const auto left_before = left.impl_->current.root;
    const auto right_before = right.impl_->current.root;

    auto left_document = co_await left.impl_->directory(left.impl_->current.root.node);
    if (!left_document)
        co_return std::unexpected(left_document.error());
    auto right_document = co_await right.impl_->directory(right.impl_->current.root.node);
    if (!right_document)
        co_return std::unexpected(right_document.error());

    auto left_sync = crdt::sync_state{};
    auto right_sync = crdt::sync_state{};
    auto quiesced = false;
    for (auto round = 0; round < 20; ++round) {
        auto progress = false;
        auto outgoing = (*left_document)->generate_sync_message(left_sync);
        if (!outgoing)
            co_return std::unexpected(outgoing.error());
        if (*outgoing) {
            auto received = (*right_document)->receive_sync_message(right_sync, **outgoing);
            if (!received)
                co_return std::unexpected(received.error());
            progress = true;
        }
        outgoing = (*right_document)->generate_sync_message(right_sync);
        if (!outgoing)
            co_return std::unexpected(outgoing.error());
        if (*outgoing) {
            auto received = (*left_document)->receive_sync_message(left_sync, **outgoing);
            if (!received)
                co_return std::unexpected(received.error());
            progress = true;
        }
        if (!progress) {
            quiesced = true;
            break;
        }
    }
    if (!quiesced)
        co_return std::unexpected(
            failure(errc::corrupt, "directory sync did not quiesce", "repository sync"));

    for (const auto& [path, left_entry] : left_before.entries) {
        auto right_entry = right_before.entries.find(path);
        if (right_entry == right_before.entries.end() ||
            left_entry.node != right_entry->second.node)
            continue;
        auto base = std::optional<digest>{};
        for (const auto& operation : left.impl_->current.operations)
            if (operation.kind == "write" && operation.from == path && operation.prior)
                base = operation.prior->object;
        for (const auto& operation : right.impl_->current.operations)
            if (operation.kind == "write" && operation.from == path && operation.prior)
                base = operation.prior->object;
        if (!base)
            continue;
        auto left_bytes = co_await left.impl_->blob_contents(left_entry.object, path);
        if (!left_bytes) {
            co_return std::unexpected(left_bytes.error());
        }
        auto right_bytes = co_await right.impl_->blob_contents(right_entry->second.object, path);
        if (!right_bytes) {
            co_return std::unexpected(right_bytes.error());
        }
        auto value = repository_detail::three_way_text_conflict(
            path, *base, left_entry.object, right_entry->second.object, *left_bytes, *right_bytes);
        if (!value)
            continue;
        auto left_stored = co_await left.impl_->store_conflict_value(*value);
        if (!left_stored) {
            co_return std::unexpected(left_stored.error());
        }
        auto right_stored = co_await right.impl_->store_conflict_value(*value);
        if (!right_stored) {
            co_return std::unexpected(right_stored.error());
        }
    }
    auto left_applied =
        apply_synced_directory(left.impl_->current, left.impl_->current.root.node, **left_document);
    if (!left_applied) {
        co_return std::unexpected(left_applied.error());
    }
    auto right_applied = apply_synced_directory(
        right.impl_->current, right.impl_->current.root.node, **right_document);
    if (!right_applied) {
        co_return std::unexpected(right_applied.error());
    }
    auto copied = co_await copy_working_objects(
        left.impl_->current.root, left.impl_->objects, right.impl_->objects);
    if (!copied) {
        co_return std::unexpected(copied.error());
    }
    copied = co_await copy_working_objects(
        right.impl_->current.root, right.impl_->objects, left.impl_->objects);
    if (!copied) {
        co_return std::unexpected(copied.error());
    }
    co_return result<void>{};
}

auto repository_detail::testing::verify_history(repository& value) -> result<std::uint64_t> {
    if (!value.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto verified = std::uint64_t{};
    for (const auto& drip : value.impl_->current.drips) {
        auto signature = identity_detail::parse_signature(drip.signature);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        const auto author =
            drip.author.taap_id.empty() ? value.impl_->signing_key.identity() : drip.author;
        auto valid = verify(author, "drip", drip.body, *signature);
        if (!valid || !*valid) {
            return std::unexpected(valid ? failure(errc::unauthorized, "unsigned drip")
                                         : valid.error());
        }
        ++verified;
    }
    if (value.impl_->current.head) {
        auto bytes = runtime::block_on(value.impl_->objects.get(value.impl_->current.head->value));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto change = objects::decode_change(*bytes, value.impl_->hash_algorithm);
        if (!change) {
            return std::unexpected(change.error());
        }
        auto body = objects::encode_change_body(*change);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto signature = identity_detail::parse_signature(change->sig);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        auto author = value.impl_->signing_key.identity();
        if (author.taap_id != change->author.taap_id) {
            const auto found =
                std::ranges::find_if(value.impl_->current.drips, [&](const auto& drip) {
                    return drip.author.taap_id == change->author.taap_id;
                });
            if (found == value.impl_->current.drips.end()) {
                return std::unexpected(
                    failure(errc::corrupt, "change author identity is unavailable"));
            }
            author = found->author;
        }
        auto valid = verify(author, "change", *body, *signature);
        if (!valid || !*valid) {
            return std::unexpected(valid ? failure(errc::unauthorized, "unsigned change")
                                         : valid.error());
        }
        ++verified;
    }
    return verified;
}

auto repository_detail::sync_access::objects(repository& source) -> store& {
    return source.impl_->objects;
}

auto repository_detail::drive_access::objects(repository& source) -> store& {
    return source.impl_->objects;
}

auto repository_detail::drive_access::fresh_node() -> node_id {
    return random_id();
}

runtime::task<digest> repository_detail::drive_access::snapshot(repository& source) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    co_return co_await source.impl_->snapshot(source.impl_->current.root, {}, false);
}

runtime::task<repository_detail::staged_drive_file>
repository_detail::drive_access::stage(repository& destination,
                                       std::filesystem::path folder,
                                       std::filesystem::path relative,
                                       std::string name,
                                       std::uint32_t mode) {
    if (!destination.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto opened =
        co_await runtime::offload([folder = std::move(folder), relative = std::move(relative)] {
            return drive_detail::source_file::open(folder, relative);
        });
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (!*opened) {
        co_return std::unexpected(opened->error());
    }
    auto content = co_await destination.impl_->objects.put_file((**opened).path());
    if (!content) {
        co_return std::unexpected(content.error());
    }
    auto value = repository_detail::store_access::blob_for(destination.impl_->objects, *content);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    value->content = {};
    auto encoded = objects::encode(*value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto reference = co_await repository_detail::store_access::put_plain(destination.impl_->objects,
                                                                         std::move(*encoded));
    if (!reference) {
        co_return std::unexpected(reference.error());
    }
    co_return staged_drive_file{tree_entry{.name = std::move(name),
                                           .mode = mode,
                                           .kind = node_kind::blob,
                                           .id = *reference,
                                           .node = random_id(),
                                           .ref = *reference},
                                *reference};
}

runtime::task<digest> repository_detail::drive_access::content(repository& source,
                                                               tree_entry entry) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (entry.kind != node_kind::blob) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "drive entry is not a file", entry.name));
    }
    auto encoded = co_await source.impl_->objects.get(entry.ref);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto value = objects::decode_blob(*encoded);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return entry.ref;
}

runtime::task<blob> repository_detail::drive_access::load_blob(repository& source,
                                                               tree_entry entry) {
    if (!source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto encoded = co_await source.impl_->objects.get(entry.ref);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return objects::decode_blob(*encoded);
}

runtime::task<void> repository_detail::drive_access::write(repository& destination,
                                                           std::string path,
                                                           tree_entry entry) {
    if (!destination.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto encoded = co_await destination.impl_->objects.get(entry.ref);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto value = objects::decode_blob(*encoded);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return co_await destination.impl_->record_blob(std::move(path),
                                                      std::move(*value),
                                                      write_options{.mode = entry.mode},
                                                      std::nullopt,
                                                      entry.node);
}

runtime::task<void>
repository_detail::drive_access::replace_conflicts(repository& destination,
                                                   std::vector<conflict> conflicts) {
    if (!destination.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    destination.impl_->current.conflict_values.clear();
    for (auto& value : conflicts) {
        auto stored = co_await destination.impl_->store_conflict_value(std::move(value));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    auto saved = co_await destination.impl_->checkpoint_or_journal();
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}

runtime::task<bool> repository_detail::drive_access::observe(repository& destination,
                                                             repository& source) {
    if (!destination.impl_ || !source.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    if (destination.impl_->current.repository != source.impl_->current.repository) {
        co_return std::unexpected(failure(errc::conflict, "drive repository identities differ"));
    }
    auto pending = std::set<node_id>{};
    append_working_directories(source.impl_->current.root, pending);
    for (;;) {
        auto progress = false;
        for (auto current = pending.begin(); current != pending.end();) {
            if (!directory_with_node(destination.impl_->current.root, *current)) {
                ++current;
                continue;
            }
            auto source_document = co_await source.impl_->directory(*current);
            auto destination_document = co_await destination.impl_->directory(*current);
            if (!source_document || !destination_document) {
                co_return std::unexpected(!source_document ? source_document.error()
                                                           : destination_document.error());
            }
            auto merged = (*destination_document)->merge(**source_document);
            if (!merged) {
                co_return std::unexpected(merged.error());
            }
            auto applied = apply_synced_directory(
                destination.impl_->current, *current, **destination_document);
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            current = pending.erase(current);
            progress = true;
        }
        if (pending.empty() || !progress) {
            break;
        }
    }
    auto checkpointed = co_await destination.impl_->checkpoint();
    if (!checkpointed) {
        co_return std::unexpected(checkpointed.error());
    }
    const auto destination_head = destination.impl_->current.head;
    const auto source_head = source.impl_->current.head;
    co_return (destination_head.has_value() != source_head.has_value()) ||
        (destination_head && source_head &&
         !same_digest(destination_head->value, source_head->value));
}

runtime::task<change_id> repository_detail::drive_access::cut(repository& destination,
                                                              repository& remote,
                                                              std::string message) {
    if (!destination.impl_ || !remote.impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto input = change_input{};
    if (destination.impl_->current.head) {
        input.parents.push_back(
            {destination.impl_->current.repository, *destination.impl_->current.head});
    }
    if (remote.impl_->current.head &&
        (!destination.impl_->current.head ||
         !same_digest(remote.impl_->current.head->value, destination.impl_->current.head->value))) {
        input.parents.push_back({remote.impl_->current.repository, *remote.impl_->current.head});
    }
    input.author = destination.impl_->signing_key.identity();
    input.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    input.message = std::move(message);
    input.ref = "main";
    co_return co_await destination.record(std::move(input));
}

auto repository_detail::search_access::index_path(repository& source)
    -> result<std::filesystem::path> {
    if (!source.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    return source.impl_->root_path / ".liquid" / "index";
}

auto repository_detail::testing::decode_drip_body(std::span<const std::byte> bytes)
    -> result<std::size_t> {
    auto decoded = decode_repository_drip(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return decoded->paths.size();
}

void repository_detail::testing::reset_directory_loads(repository& value) {
    if (value.impl_)
        value.impl_->directory_load_count = 0;
}

auto repository_detail::testing::rename_working_entry(repository& value,
                                                      const std::string& from,
                                                      std::string to) -> bool {
    if (!value.impl_)
        return false;
    auto& entries = value.impl_->current.root.entries;
    auto found = entries.find(from);
    if (found == entries.end())
        return false;
    auto entry = std::move(found->second);
    entries.erase(found);
    return entries.emplace(std::move(to), std::move(entry)).second;
}

auto repository_detail::shared_state_prefix(const std::filesystem::path& identifier)
    -> result<std::string> {
    auto key = identifier_key(identifier);
    if (!key) {
        return std::unexpected(key.error());
    }
    return *key + "/";
}
} // namespace liquid
