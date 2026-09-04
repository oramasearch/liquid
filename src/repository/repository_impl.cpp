#include "liquid/repository.hpp"
#include "liquid/store.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include "crdt/document.hpp"
#include "encoding/cbor.hpp"
#include "encryption/leaf.hpp"
#include "formats/dotenv.hpp"
#include "formats/json.hpp"
#include "identity/taap.hpp"
#include "objects/records.hpp"
#include "repository/merge.hpp"
#include "repository/store_access.hpp"
#include "repository/testing.hpp"
#include "store/pack.hpp"
#include "vfs/repository_access.hpp"

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
};
struct stored_operation {
    op_id id{};
    std::string kind;
    std::string from;
    std::string to;
    node_id node{};
    std::optional<working_entry> prior;
    actor by;
    std::chrono::sys_seconds at;
};
struct persisted_drip {
    std::vector<std::byte> body;
    std::vector<std::byte> signature;
};
struct repository_document {
    node_id repository{};
    working_entry root{entry_kind::directory, {}, 0040755, {}, {}};
    std::map<std::string, change_id> refs;
    std::map<node_id, digest> dirs;
    std::optional<change_id> head;
    std::vector<stored_operation> operations;
    std::vector<std::string> locks;
    std::vector<conflict> conflict_values;
    std::vector<persisted_drip> drips;
    std::vector<encryption_path_rule> encryption_rules;
    std::uint64_t pending_drips{};
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
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
auto encode_entry(const working_entry& entry) -> cbor::value {
    auto children = cbor::value::array{};
    for (const auto& [name, child] : entry.entries)
        children.push_back(map({{"name", cbor::value{name}}, {"entry", encode_entry(child)}}));
    return map({{"kind", cbor::value{static_cast<std::uint64_t>(entry.kind)}},
                {"node", bytes(entry.node)},
                {"mode", cbor::value{std::uint64_t{entry.mode}}},
                {"object", digest_value(entry.object)},
                {"entries", cbor::value{std::move(children)}}});
}
auto decode_entry(const cbor::value& value) -> result<working_entry> {
    const auto* kind = find(value, "kind");
    const auto* node = find(value, "node");
    const auto* mode = find(value, "mode");
    const auto* object = find(value, "object");
    const auto* children = find(value, "entries");
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
    auto dirs = cbor::value::array{};
    for (const auto& [node, document] : value.dirs)
        dirs.push_back(map({{"node", bytes(node)}, {"document", digest_value(document)}}));
    auto ops = cbor::value::array{};
    for (const auto& operation : value.operations)
        ops.push_back(
            map({{"id", digest_value(operation.id.value)},
                 {"kind", cbor::value{operation.kind}},
                 {"from", cbor::value{operation.from}},
                 {"to", cbor::value{operation.to}},
                 {"node", bytes(operation.node)},
                 {"by", actor_value(operation.by)},
                 {"at", cbor::value{operation.at.time_since_epoch().count()}},
                 {"prior",
                  operation.prior ? encode_entry(*operation.prior) : cbor::value{cbor::null{}}}}));
    auto locks = cbor::value::array{};
    for (const auto& lock : value.locks)
        locks.push_back(cbor::value{lock});
    auto head = value.head ? digest_value(value.head->value) : cbor::value{cbor::null{}};
    auto drips = cbor::value::array{};
    for (const auto& drip : value.drips) {
        drips.push_back(map({{"body", bytes(drip.body)}, {"sig", bytes(drip.signature)}}));
    }
    auto encryption_paths = cbor::value::array{};
    for (const auto& rule : value.encryption_rules) {
        encryption_paths.push_back(
            map({{"path", cbor::value{rule.path}},
                 {"key_id", cbor::value{rule.key_id}},
                 {"mode", cbor::value{rule.mode == encryption_mode::leaf ? "leaf" : "file"}},
                 {"encrypted_regex", cbor::value{rule.encrypted_regex}}}));
    }
    auto config = map({{"encrypt", map({{"paths", cbor::value{std::move(encryption_paths)}}})}});
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "repository-document"},
                             {"repo", bytes(value.repository)},
                             {"root", encode_entry(value.root)},
                             {"refs", cbor::value{std::move(refs)}},
                             {"dirs", cbor::value{std::move(dirs)}},
                             {"ops", cbor::value{std::move(ops)}},
                             {"head", std::move(head)},
                             {"locks", cbor::value{std::move(locks)}},
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
    const auto* refs = find(*decoded, "refs");
    const auto* dirs = find(*decoded, "dirs");
    const auto* ops = find(*decoded, "ops");
    const auto* head = find(*decoded, "head");
    const auto* locks = find(*decoded, "locks");
    const auto* config = find(*decoded, "config");
    const auto* drip_values = find(*decoded, "drips");
    const auto* pending = find(*decoded, "pending");
    const auto* v = version ? std::get_if<std::uint64_t>(&version->data) : nullptr;
    const auto* title = kind ? std::get_if<std::string>(&kind->data) : nullptr;
    const auto* id = repo ? std::get_if<cbor::value::bytes>(&repo->data) : nullptr;
    const auto* ref_values = refs ? std::get_if<cbor::value::array>(&refs->data) : nullptr;
    const auto* dir_values = dirs ? std::get_if<cbor::value::array>(&dirs->data) : nullptr;
    const auto* op_values = ops ? std::get_if<cbor::value::array>(&ops->data) : nullptr;
    const auto* lock_values = locks ? std::get_if<cbor::value::array>(&locks->data) : nullptr;
    const auto* saved_drips =
        drip_values ? std::get_if<cbor::value::array>(&drip_values->data) : nullptr;
    const auto* drips = pending ? std::get_if<std::uint64_t>(&pending->data) : nullptr;
    if (!v || *v != 1 || !title || *title != "repository-document" || !id || id->size() != 16 ||
        !root || !ref_values || !dir_values || !op_values || !head || !lock_values ||
        !saved_drips || !drips)
        return std::unexpected(
            failure(errc::corrupt, "invalid repository document", "LIQUID_REPO"));
    auto root_entry = decode_entry(*root);
    if (!root_entry)
        return std::unexpected(root_entry.error());
    repository_document output{};
    std::ranges::copy(*id, output.repository.begin());
    output.root = std::move(*root_entry);
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
    for (const auto& item : *op_values) {
        const auto* operation_id = find(item, "id");
        const auto* operation_kind = find(item, "kind");
        const auto* from = find(item, "from");
        const auto* to = find(item, "to");
        const auto* node = find(item, "node");
        const auto* by = find(item, "by");
        const auto* at = find(item, "at");
        const auto* prior = find(item, "prior");
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
        output.operations.push_back({op_id{*digest},
                                     *kind_text,
                                     *from_text,
                                     *to_text,
                                     identity,
                                     std::move(previous),
                                     std::move(operation_actor),
                                     operation_time});
    }
    if (!std::holds_alternative<cbor::null>(head->data)) {
        auto digest = decode_digest(*head);
        if (!digest)
            return std::unexpected(digest.error());
        output.head = change_id{*digest};
    }
    for (const auto& item : *lock_values) {
        const auto* text = std::get_if<std::string>(&item.data);
        if (!text)
            return std::unexpected(failure(errc::corrupt, "invalid lock", "repository document"));
        output.locks.push_back(*text);
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
    }
    for (const auto& item : *saved_drips) {
        const auto* body = find(item, "body");
        const auto* signature = find(item, "sig");
        const auto* body_bytes = body ? std::get_if<cbor::value::bytes>(&body->data) : nullptr;
        const auto* signature_bytes =
            signature ? std::get_if<cbor::value::bytes>(&signature->data) : nullptr;
        if (!body_bytes || !signature_bytes) {
            return std::unexpected(
                failure(errc::corrupt, "invalid drip record", "repository document"));
        }
        output.drips.push_back({*body_bytes, *signature_bytes});
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
auto same_path_or_descendant(std::string_view path, std::string_view root) -> bool {
    return path == root ||
           (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
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
} // namespace

struct repository::impl {
    std::filesystem::path root_path;
    store objects;
    repository_document current;
    std::uint64_t checkpoint_interval{64};
    algorithm hash_algorithm;
    std::uint64_t directory_load_count{};
    std::map<node_id, crdt::directory_document> directory_cache;
    signer signing_key;
    std::shared_ptr<keyring> keys;
    impl(std::filesystem::path path,
         store value,
         repository_document document,
         signer key,
         std::shared_ptr<keyring> encryption_keys,
         std::uint64_t interval,
         algorithm algorithm_value)
        : root_path(std::move(path)), objects(std::move(value)), current(std::move(document)),
          checkpoint_interval(std::max(std::uint64_t{1}, interval)),
          hash_algorithm(algorithm_value), signing_key(std::move(key)),
          keys(std::move(encryption_keys)) {}
    auto marker() const -> std::filesystem::path {
        return root_path / "LIQUID_REPO";
    }
    auto journal_marker() const -> std::filesystem::path {
        return root_path / "LIQUID_JOURNAL";
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
        if (found != directory_cache.end())
            co_return &found->second;
        if (auto saved = current.dirs.find(id); saved != current.dirs.end()) {
            auto raw = co_await objects.get(saved->second);
            if (!raw)
                co_return std::unexpected(raw.error());
            auto loaded = crdt::directory_document::load(*raw, "local", "repository");
            if (!loaded)
                co_return std::unexpected(loaded.error());
            auto inserted = directory_cache.emplace(id, std::move(*loaded));
            ++directory_load_count;
            auto restored = co_await restore_directory_document(id, inserted.first->second);
            if (!restored)
                co_return std::unexpected(restored.error());
            co_return &inserted.first->second;
        }
        crdt::repo_id repository{};
        std::ranges::copy(current.repository, repository.begin());
        auto created = crdt::directory_document::create(repository, "local", "repository");
        if (!created)
            co_return std::unexpected(created.error());
        auto inserted = directory_cache.emplace(id, std::move(*created));
        auto restored = co_await restore_directory_document(id, inserted.first->second);
        if (!restored)
            co_return std::unexpected(restored.error());
        co_return &inserted.first->second;
    }
    runtime::task<void> persist() {
        for (auto& [node, document] : directory_cache) {
            auto saved_document = document.save();
            if (!saved_document)
                co_return std::unexpected(saved_document.error());
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
        auto temporary = marker();
        temporary += ".tmp";
        const auto text = to_string(*stored) + "\n";
        auto marker_bytes =
            std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                   reinterpret_cast<const std::byte*>(text.data() + text.size())};
        auto written = co_await store_detail::write_file(temporary, std::move(marker_bytes));
        if (!written)
            co_return std::unexpected(written.error());
        auto code = std::error_code{};
        std::filesystem::rename(temporary, marker(), code);
        if (code) {
            std::filesystem::remove(marker(), code);
            code.clear();
            std::filesystem::rename(temporary, marker(), code);
        }
        if (code)
            co_return std::unexpected(failure(errc::io, code.message(), marker().string()));
        co_return result<void>{};
    }
    runtime::task<void> checkpoint_or_journal() {
        if (current.pending_drips >= checkpoint_interval)
            co_return co_await persist();
        auto encoded = encode_repository_document(current);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        co_return co_await store_detail::write_file(journal_marker(), std::move(*encoded));
    }
    runtime::task<void> append_drip(std::string operation, const node_id& node) {
        const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto body = cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                                      {"kind", "drip"},
                                      {"author", cbor::value{signing_key.identity().taap_id}},
                                      {"at", cbor::value{at.time_since_epoch().count()}},
                                      {"node", bytes(node)},
                                      {"operation", cbor::value{std::move(operation)}}}));
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
        current.drips.push_back({std::move(*body), std::move(*encoded_signature)});
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
        current.drips.push_back({std::move(*body), std::move(*encoded_signature)});
        co_return result<void>{};
    }
    void record_operation(op_id id,
                          std::string kind,
                          std::string from,
                          std::string to,
                          node_id node,
                          std::optional<working_entry> prior) {
        current.operations.push_back(
            {std::move(id),
             std::move(kind),
             std::move(from),
             std::move(to),
             node,
             std::move(prior),
             signing_key.identity(),
             std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())});
    }
    runtime::task<digest> snapshot(working_entry& entry) {
        tree value{};
        for (auto& [name, child] : entry.entries) {
            digest reference = child.object;
            node_kind kind = node_kind::blob;
            if (child.kind == entry_kind::directory) {
                auto nested = co_await snapshot(child);
                if (!nested)
                    co_return std::unexpected(nested.error());
                reference = *nested;
                child.object = reference;
                kind = node_kind::tree;
            }
            if (child.kind == entry_kind::nested_repository)
                kind = node_kind::repo;
            value.entries.push_back({name, child.mode, kind, reference, child.node, reference});
        }
        auto encoded = objects::encode(value);
        if (!encoded)
            co_return std::unexpected(encoded.error());
        co_return co_await repository_detail::store_access::put_plain(objects, std::move(*encoded));
    }
    runtime::task<tree> root_tree() {
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
    auto locked(std::string_view path) const -> bool {
        return std::ranges::any_of(current.locks, [&](const auto& pattern) {
            return same_path_or_descendant(path, pattern);
        });
    }
    runtime::task<void> record_blob(std::string path, blob record) {
        if (locked(path))
            co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
        auto parts = split_path(path);
        if (!parts)
            co_return std::unexpected(parts.error());
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
        const auto existing_id = found == (*parent)->entries.end() ? node_id{} : found->second.node;
        auto recorded =
            (*directory_document)
                ->put_entry(parts->back(), {"blob", existing_id, 0100644, to_string(*reference)});
        if (!recorded)
            co_return std::unexpected(recorded.error());
        if (found != (*parent)->entries.end()) {
            prior = found->second;
            found->second.object = *reference;
        } else {
            found =
                (*parent)
                    ->entries
                    .emplace(parts->back(),
                             working_entry{entry_kind::file, random_id(), 0100644, *reference, {}})
                    .first;
        }
        std::erase_if(current.conflict_values,
                      [&](const conflict& value) { return value.path == path; });
        auto operation = random_operation_id(hash_algorithm);
        if (!operation)
            co_return std::unexpected(operation.error());
        record_operation(*operation, "write", path, {}, found->second.node, std::move(prior));
        auto signed_drip = co_await append_drip("write", found->second.node);
        if (!signed_drip)
            co_return std::unexpected(signed_drip.error());
        ++current.pending_drips;
        auto saved = co_await checkpoint_or_journal();
        if (!saved)
            co_return std::unexpected(saved.error());
        co_return result<void>{};
    }
};
repository::repository() noexcept = default;
repository::~repository() = default;
repository::repository(repository&&) noexcept = default;
auto repository::operator=(repository&&) noexcept -> repository& = default;
runtime::task<repository> repository::create(std::filesystem::path path,
                                             repository_options options) {
    auto valid_rules = validate_encryption_rules(options.encrypt.paths);
    if (!valid_rules) {
        co_return std::unexpected(valid_rules.error());
    }
    auto code = std::error_code{};
    std::filesystem::create_directories(path, code);
    if (code)
        co_return std::unexpected(failure(errc::io, code.message(), path.string()));
    if (std::filesystem::exists(path / "LIQUID_REPO"))
        co_return std::unexpected(
            failure(errc::already_exists, "repository already exists", path.string()));
    const auto hash_algorithm = options.store.hash_algorithm;
    auto keys = options.store.keys;
    auto objects = co_await store::open(path / ".liquid-store", std::move(options.store));
    if (!objects)
        co_return std::unexpected(objects.error());
    repository_document document{};
    document.repository = random_id();
    document.root.node = random_id();
    document.root.object.alg = hash_algorithm;
    document.encryption_rules = std::move(options.encrypt.paths);
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key)
        co_return std::unexpected(signing_key.error());
    repository output;
    output.impl_ = std::make_unique<impl>(std::move(path),
                                          std::move(*objects),
                                          std::move(document),
                                          std::move(*signing_key),
                                          std::move(keys),
                                          options.checkpoint_interval,
                                          hash_algorithm);
    auto saved = co_await output.impl_->persist();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return output;
}
runtime::task<repository> repository::open(std::filesystem::path path, repository_options options) {
    auto marker = co_await store_detail::read_file(path / "LIQUID_REPO");
    if (!marker)
        co_return std::unexpected(marker.error());
    auto text = std::string{reinterpret_cast<const char*>(marker->data()), marker->size()};
    if (const auto newline = text.find('\n'); newline != std::string::npos)
        text.resize(newline);
    auto document_id = parse_digest(text);
    if (!document_id)
        co_return std::unexpected(document_id.error());
    const auto hash_algorithm = options.store.hash_algorithm;
    auto keys = options.store.keys;
    auto objects = co_await store::open(path / ".liquid-store", std::move(options.store));
    if (!objects)
        co_return std::unexpected(objects.error());
    auto saved = co_await objects->get(*document_id);
    if (!saved)
        co_return std::unexpected(saved.error());
    auto document = decode_repository_document(*saved);
    if (!document)
        co_return std::unexpected(document.error());
    if (std::filesystem::exists(path / "LIQUID_JOURNAL")) {
        auto journal = co_await store_detail::read_file(path / "LIQUID_JOURNAL");
        if (!journal)
            co_return std::unexpected(journal.error());
        auto replayed = decode_repository_document(*journal);
        if (!replayed)
            co_return std::unexpected(replayed.error());
        document = std::move(replayed);
    }
    auto valid_rules = validate_encryption_rules(document->encryption_rules);
    if (!valid_rules)
        co_return std::unexpected(valid_rules.error());
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key)
        co_return std::unexpected(signing_key.error());
    repository output;
    output.impl_ = std::make_unique<impl>(std::move(path),
                                          std::move(*objects),
                                          std::move(*document),
                                          std::move(*signing_key),
                                          std::move(keys),
                                          options.checkpoint_interval,
                                          hash_algorithm);
    co_return output;
}
runtime::task<tree> repository::root() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return co_await impl_->root_tree();
}
runtime::task<change_id> repository::head() {
    if (!impl_ || !impl_->current.head)
        co_return std::unexpected(failure(errc::not_found, "repository has no change"));
    co_return *impl_->current.head;
}
runtime::task<std::vector<std::byte>> repository::read(std::string path) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto* entry = lookup(impl_->current.root, *parts);
    if (!entry || entry->kind != entry_kind::file)
        co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
    co_return co_await impl_->blob_contents(entry->object, std::move(path));
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
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (impl_->locked(path))
        co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
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
    co_return co_await impl_->record_blob(std::move(path), std::move(*record));
}
runtime::task<void> repository::write_file(std::string path, std::filesystem::path source) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (encryption_rule_for(impl_->current, path)) {
        auto contents = co_await store_detail::read_file(source);
        if (!contents)
            co_return std::unexpected(contents.error());
        co_return co_await write(std::move(path), std::move(*contents));
    }
    auto content = co_await impl_->objects.put_file(std::move(source));
    if (!content)
        co_return std::unexpected(content.error());
    auto record = impl_->objects.blob_for(*content);
    if (!record)
        co_return std::unexpected(record.error());
    co_return co_await impl_->record_blob(std::move(path), std::move(*record));
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
    impl_->current.operations.push_back(
        {op_id{*operation_hash}, "redaction", path, {}, current->node, prior});
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
runtime::task<void> repository::move(std::string from, std::string to) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (impl_->locked(from) || impl_->locked(to))
        co_return std::unexpected(failure(errc::locked, "path is advisory locked", from));
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
    auto source_document = co_await impl_->directory((*source_parent)->node);
    if (!source_document)
        co_return std::unexpected(source_document.error());
    if ((*source_parent)->node == (*destination_parent)->node) {
        auto recorded = (*source_document)->rename_entry(source->back(), destination->back());
        if (!recorded)
            co_return std::unexpected(recorded.error());
    } else {
        auto destination_document = co_await impl_->directory((*destination_parent)->node);
        if (!destination_document)
            co_return std::unexpected(destination_document.error());
        auto removed = (*source_document)->remove_entry(source->back());
        if (!removed)
            co_return std::unexpected(removed.error());
        const auto kind = found->second.kind == entry_kind::file        ? "blob"
                          : found->second.kind == entry_kind::directory ? "tree"
                                                                        : "repo";
        auto inserted = (*destination_document)
                            ->put_entry(destination->back(),
                                        {kind,
                                         found->second.node,
                                         found->second.mode,
                                         to_string(found->second.object)});
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
    auto signed_drip = co_await impl_->append_drip("move", moved_node);
    if (!signed_drip)
        co_return std::unexpected(signed_drip.error());
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<void> repository::remove(std::string path) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (impl_->locked(path))
        co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, false, impl_->hash_algorithm);
    if (!parent)
        co_return std::unexpected(parent.error());
    auto found = (*parent)->entries.find(parts->back());
    if (found == (*parent)->entries.end())
        co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
    auto directory = co_await impl_->directory((*parent)->node);
    if (!directory)
        co_return std::unexpected(directory.error());
    auto recorded = (*directory)->remove_entry(parts->back());
    if (!recorded)
        co_return std::unexpected(recorded.error());
    auto prior = found->second;
    (*parent)->entries.erase(found);
    auto operation = random_operation_id(impl_->hash_algorithm);
    if (!operation)
        co_return std::unexpected(operation.error());
    impl_->record_operation(*operation, "remove", path, {}, prior.node, std::move(prior));
    auto signed_drip = co_await impl_->append_drip("remove", prior.node);
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
    auto root = co_await impl_->snapshot(impl_->current.root);
    if (!root)
        co_return std::unexpected(root.error());
    change value{};
    if (impl_->current.head)
        value.parents = {*impl_->current.head};
    value.author = impl_->signing_key.identity();
    value.message = std::move(message);
    value.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    value.root = *root;
    value.conflicts = impl_->current.conflict_values;
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
    change_id id{*stored};
    impl_->current.head = id;
    impl_->current.refs["main"] = id;
    impl_->current.pending_drips = 0;
    auto saved = co_await impl_->persist();
    if (!saved)
        co_return std::unexpected(saved.error());
    std::error_code cleanup;
    std::filesystem::remove(impl_->journal_marker(), cleanup);
    co_return id;
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
    impl_->current.refs[std::move(name)] = id;
    auto saved = co_await impl_->checkpoint_or_journal();
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
    auto visit = [&](auto&& self, const working_entry& entry, std::string path) -> void {
        if (entry.kind == entry_kind::directory && entry.entries.size() > 10000)
            output.oversized_directories.push_back(path);
        for (const auto& [name, child] : entry.entries)
            self(self, child, path.empty() ? name : path + "/" + name);
    };
    visit(visit, impl_->current.root, "");
    return output;
}
runtime::task<std::vector<change>> repository::log(log_options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    std::vector<change> output;
    auto next = impl_->current.head;
    while (next) {
        auto bytes = co_await impl_->objects.get(next->value);
        if (!bytes)
            co_return std::unexpected(bytes.error());
        auto value = objects::decode_change(*bytes, impl_->hash_algorithm);
        if (!value)
            co_return std::unexpected(value.error());
        output.push_back(*value);
        next = value->parents.empty() ? std::optional<change_id>{}
                                      : std::optional<change_id>{value->parents.front()};
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
                                          : std::optional<change_id>{value->parents.front()};
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
    auto nested = source.impl_->current.root;
    nested.kind = entry_kind::nested_repository;
    (*parent)->entries.emplace(parts->back(), std::move(nested));
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<repository> repository::split(std::string path, std::filesystem::path into) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    auto* entry = lookup(impl_->current.root, *parts);
    if (!entry || entry->kind != entry_kind::directory)
        co_return std::unexpected(failure(errc::not_found, "subtree does not exist", path));
    repository_options options;
    options.checkpoint_interval = impl_->checkpoint_interval;
    options.store.hash_algorithm = impl_->hash_algorithm;
    auto destination = co_await create(std::move(into), std::move(options));
    if (!destination)
        co_return std::unexpected(destination.error());
    destination->impl_->current.root = *entry;
    auto child_change = co_await destination->cut("split subtree");
    if (!child_change)
        co_return std::unexpected(child_change.error());
    entry->kind = entry_kind::nested_repository;
    entry->object = child_change->value;
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return std::move(*destination);
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
    if ((*parent)->entries.contains(parts->back())) {
        if (auto value =
                repository_detail::entry_conflict(path,
                                                  (*parent)->entries.at(parts->back()).object,
                                                  other.impl_->current.root.object))
            impl_->current.conflict_values.push_back(std::move(*value));
    } else {
        auto copy = other.impl_->current.root;
        copy.kind = entry_kind::nested_repository;
        (*parent)->entries.emplace(parts->back(), std::move(copy));
    }
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<std::vector<conflict>> repository::conflicts() {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    co_return impl_->current.conflict_values;
}
runtime::task<std::vector<operation_record>> repository::operations(log_options) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto output = std::vector<operation_record>{};
    output.reserve(impl_->current.operations.size());
    for (auto operation = impl_->current.operations.rbegin();
         operation != impl_->current.operations.rend();
         ++operation) {
        output.push_back({operation->id,
                          operation->kind,
                          operation->from,
                          operation->to,
                          operation->by,
                          operation->at});
    }
    co_return output;
}
runtime::task<void> repository::undo(op_id operation) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto found = std::ranges::find_if(impl_->current.operations, [&](const auto& value) {
        return value.id.value.alg == operation.value.alg &&
               value.id.value.bytes == operation.value.bytes;
    });
    if (found == impl_->current.operations.end())
        co_return std::unexpected(failure(errc::not_found, "operation does not exist"));
    if (found->kind == "move") {
        auto result = co_await move(found->to, found->from);
        if (!result)
            co_return std::unexpected(result.error());
    } else if (found->kind == "remove" && found->prior) {
        auto parts = split_path(found->from);
        if (!parts)
            co_return std::unexpected(parts.error());
        auto parent = parent_for(impl_->current.root, *parts, true, impl_->hash_algorithm);
        if (!parent)
            co_return std::unexpected(parent.error());
        (*parent)->entries[parts->back()] = *found->prior;
        ++impl_->current.pending_drips;
        auto saved = co_await impl_->checkpoint_or_journal();
        if (!saved)
            co_return std::unexpected(saved.error());
    } else if (found->kind == "write") {
        auto parts = split_path(found->from);
        if (!parts)
            co_return std::unexpected(parts.error());
        auto parent = parent_for(impl_->current.root, *parts, false, impl_->hash_algorithm);
        if (!parent)
            co_return std::unexpected(parent.error());
        if (found->prior)
            (*parent)->entries[parts->back()] = *found->prior;
        else
            (*parent)->entries.erase(parts->back());
        ++impl_->current.pending_drips;
        auto saved = co_await impl_->checkpoint_or_journal();
        if (!saved)
            co_return std::unexpected(saved.error());
    } else
        co_return std::unexpected(failure(errc::unsupported, "operation cannot be undone"));
    co_return result<void>{};
}
runtime::task<void> repository::lock(std::string path) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    auto parts = split_path(path);
    if (!parts)
        co_return std::unexpected(parts.error());
    if (std::ranges::find(impl_->current.locks, path) == impl_->current.locks.end())
        impl_->current.locks.push_back(std::move(path));
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}
runtime::task<void> repository::unlock(std::string path) {
    if (!impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    std::erase(impl_->current.locks, path);
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return result<void>{};
}

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
        output.size = record->size;
        output.chunks = std::move(record->chunks);
        output.chunk_sizes = std::move(record->chunk_sizes);
    }
    co_return output;
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
            value.size = record->size;
            value.chunks = std::move(record->chunks);
            value.chunk_sizes = std::move(record->chunk_sizes);
        }
        output.push_back(std::move(value));
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
    if (source.impl_->locked(path))
        co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
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
    auto recorded =
        (*directory)->put_entry(parts->back(), {"tree", identity, mode, to_string(reference)});
    if (!recorded)
        co_return std::unexpected(recorded.error());
    (*parent)->entries.emplace(parts->back(),
                               working_entry{entry_kind::directory, identity, mode, reference, {}});
    auto operation = random_operation_id(source.impl_->hash_algorithm);
    if (!operation)
        co_return std::unexpected(operation.error());
    source.impl_->record_operation(*operation, "mkdir", path, {}, identity, std::nullopt);
    auto signed_drip = co_await source.impl_->append_drip("mkdir", identity);
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
    options.store.hash_algorithm = source.impl_->hash_algorithm;
    co_return co_await repository::open(std::move(destination), std::move(options));
}

runtime::task<void> repository_detail::testing::sync(repository& left, repository& right) {
    if (!left.impl_ || !right.impl_)
        co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    if (left.impl_->current.repository != right.impl_->current.repository)
        co_return std::unexpected(failure(errc::invalid_argument, "repository identities differ"));

    auto left_document = co_await left.impl_->directory(left.impl_->current.root.node);
    if (!left_document)
        co_return std::unexpected(left_document.error());
    auto right_document = co_await right.impl_->directory(right.impl_->current.root.node);
    if (!right_document)
        co_return std::unexpected(right_document.error());

    auto left_sync = crdt::sync_state{};
    auto right_sync = crdt::sync_state{};
    auto outgoing = (*left_document)->generate_sync_message(left_sync);
    if (!outgoing)
        co_return std::unexpected(outgoing.error());
    if (*outgoing) {
        auto received = (*right_document)->receive_sync_message(right_sync, **outgoing);
        if (!received)
            co_return std::unexpected(received.error());
    }
    outgoing = (*right_document)->generate_sync_message(right_sync);
    if (!outgoing)
        co_return std::unexpected(outgoing.error());
    if (*outgoing) {
        auto received = (*left_document)->receive_sync_message(left_sync, **outgoing);
        if (!received)
            co_return std::unexpected(received.error());
    }

    for (const auto& [path, left_entry] : left.impl_->current.root.entries) {
        auto right_entry = right.impl_->current.root.entries.find(path);
        if (right_entry == right.impl_->current.root.entries.end() ||
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
        auto value = repository_detail::three_way_entry_conflict(
            path, *base, left_entry.object, right_entry->second.object);
        if (!value)
            continue;
        left.impl_->current.conflict_values.push_back(*value);
        right.impl_->current.conflict_values.push_back(*value);
        auto winner = (*left_document)->entry_at(path);
        if (!winner)
            co_return std::unexpected(winner.error());
        if (*winner) {
            auto ref = parse_digest((*winner)->ref);
            if (!ref)
                co_return std::unexpected(ref.error());
            left.impl_->current.root.entries[path].object = *ref;
            right.impl_->current.root.entries[path].object = *ref;
        }
    }
    co_return result<void>{};
}

auto repository_detail::testing::verify_history(repository& value) -> result<std::uint64_t> {
    if (!value.impl_) {
        return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
    }
    auto verified = std::uint64_t{};
    const auto author = value.impl_->signing_key.identity();
    for (const auto& drip : value.impl_->current.drips) {
        auto signature = identity_detail::parse_signature(drip.signature);
        if (!signature) {
            return std::unexpected(signature.error());
        }
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
        if (change->author.taap_id != author.taap_id) {
            return std::unexpected(
                failure(errc::unauthorized, "change author identity is unavailable"));
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

void repository_detail::testing::reset_directory_loads(repository& value) {
    if (value.impl_)
        value.impl_->directory_load_count = 0;
}
} // namespace liquid
