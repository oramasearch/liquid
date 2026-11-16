// SPDX-License-Identifier: MIT

#include "crdt/schema.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>

#include <automerge-cpp/transaction.hpp>

namespace liquid::crdt {
namespace {

namespace am = automerge_cpp;

constexpr auto repo_key = std::string_view{"repo"};
constexpr auto format_key = std::string_view{"format"};
constexpr auto tree_key = std::string_view{"tree"};
constexpr auto refs_key = std::string_view{"refs"};
constexpr auto locks_key = std::string_view{"locks"};
constexpr auto nested_key = std::string_view{"nested"};
constexpr auto config_key = std::string_view{"config"};
constexpr auto ops_key = std::string_view{"ops"};
constexpr auto kind_key = std::string_view{"kind"};
constexpr auto id_key = std::string_view{"id"};
constexpr auto mode_key = std::string_view{"mode"};
constexpr auto ref_key = std::string_view{"ref"};
constexpr auto path_key = std::string_view{"path"};
constexpr auto to_key = std::string_view{"to"};
constexpr auto by_key = std::string_view{"by"};

auto invalid(std::string message, std::string context = {}) -> error {
    return error{errc::invalid_argument, std::move(message), std::move(context)};
}

auto corrupt(std::string message, std::string context = {}) -> error {
    return error{errc::corrupt, std::move(message), std::move(context)};
}

auto automerge_error(const am::Error& source, errc code, std::string context) -> error {
    auto message = source.message;
    if (message.empty())
        message = std::string{am::to_string_view(source.kind)};
    return error{code, std::move(message), std::move(context)};
}

template <typename T>
auto checked(std::expected<T, am::Error> source, errc code, std::string context) -> result<T> {
    if (!source)
        return std::unexpected(automerge_error(source.error(), code, std::move(context)));
    return std::move(*source);
}

auto bytes(const node_id& id) -> am::Bytes {
    return am::Bytes{id.begin(), id.end()};
}

auto fixed_id(const std::optional<am::Bytes>& input, std::string_view field) -> result<node_id> {
    if (!input || input->size() != node_id{}.size()) {
        return std::unexpected(corrupt("expected 16-byte identifier", std::string{field}));
    }
    auto output = node_id{};
    std::ranges::copy(*input, output.begin());
    return output;
}

auto split_path(std::string_view path) -> result<std::vector<std::string>> {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return std::unexpected(invalid("path must be relative and non-empty", std::string{path}));
    }
    auto output = std::vector<std::string>{};
    for (std::size_t begin = 0; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto part =
            path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..") {
            return std::unexpected(
                invalid("path contains an invalid component", std::string{path}));
        }
        output.emplace_back(part);
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return output;
}

auto tree(const am::Document& document) -> result<std::optional<am::ObjId>> {
    return checked(document.get_obj_id(am::root, tree_key), errc::corrupt, "read tree root");
}

auto child(const am::Document& document, const am::ObjId& parent, std::string_view key)
    -> result<std::optional<am::ObjId>> {
    return checked(document.get_obj_id(parent, key), errc::corrupt, "read child object");
}

auto is_entry(const am::Document& document, const am::ObjId& object) -> result<bool> {
    auto kind =
        checked(document.get<std::string>(object, kind_key), errc::corrupt, "read entry kind");
    if (!kind)
        return std::unexpected(kind.error());
    return kind->has_value();
}

auto find_object(const am::Document& document, const std::vector<std::string>& components)
    -> result<std::optional<am::ObjId>> {
    auto current = tree(document);
    if (!current)
        return std::unexpected(current.error());
    if (!*current)
        return std::optional<am::ObjId>{};
    for (const auto& component : components) {
        current = child(document, **current, component);
        if (!current)
            return std::unexpected(current.error());
        if (!*current)
            return std::optional<am::ObjId>{};
    }
    return *current;
}

auto find_parent(const am::Document& document, const std::vector<std::string>& components)
    -> result<std::optional<am::ObjId>> {
    if (components.empty())
        return std::optional<am::ObjId>{};
    return find_object(document,
                       std::vector<std::string>{components.begin(), components.end() - 1});
}

auto validate_write(const am::Document& document, const operation& op) -> result<void> {
    auto components = split_path(op.path);
    if (!components)
        return std::unexpected(components.error());
    if (op.ref.empty())
        return std::unexpected(invalid("write reference is empty", op.path));
    if (op.value_kind != "blob" && op.value_kind != "tree" && op.value_kind != "repo")
        return std::unexpected(invalid("write kind is invalid", op.path));
    auto current = tree(document);
    if (!current)
        return std::unexpected(current.error());
    if (!*current)
        return std::unexpected(corrupt("tree root is missing"));
    for (std::size_t i = 0; i + 1 < components->size(); ++i) {
        auto next = child(document, **current, (*components)[i]);
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            break;
        auto entry = is_entry(document, **next);
        if (!entry)
            return std::unexpected(entry.error());
        if (*entry) {
            return std::unexpected(invalid("path component is not a directory", op.path));
        }
        current = *next;
    }
    auto existing = find_object(document, *components);
    if (!existing)
        return std::unexpected(existing.error());
    if (*existing) {
        auto entry = is_entry(document, **existing);
        if (!entry)
            return std::unexpected(entry.error());
        if (!*entry)
            return std::unexpected(invalid("cannot overwrite a directory", op.path));
    }
    return {};
}

auto validate_move(const am::Document& document, const operation& op) -> result<void> {
    auto source = split_path(op.path);
    if (!source)
        return std::unexpected(source.error());
    auto destination = split_path(op.to);
    if (!destination)
        return std::unexpected(destination.error());
    if (op.path == op.to || op.to.starts_with(op.path + "/")) {
        return std::unexpected(invalid("move destination is inside its source", op.to));
    }
    auto source_object = find_object(document, *source);
    if (!source_object)
        return std::unexpected(source_object.error());
    if (!*source_object) {
        return std::unexpected(invalid("move source does not exist", op.path));
    }
    auto destination_object = find_object(document, *destination);
    if (!destination_object)
        return std::unexpected(destination_object.error());
    if (*destination_object) {
        return std::unexpected(invalid("move destination already exists", op.to));
    }
    auto destination_parent = find_parent(document, *destination);
    if (!destination_parent)
        return std::unexpected(destination_parent.error());
    if (!*destination_parent) {
        return std::unexpected(invalid("move destination parent does not exist", op.to));
    }
    return {};
}

auto validate_remove(const am::Document& document, const operation& op) -> result<void> {
    auto components = split_path(op.path);
    if (!components)
        return std::unexpected(components.error());
    auto object = find_object(document, *components);
    if (!object)
        return std::unexpected(object.error());
    if (!*object) {
        return std::unexpected(invalid("remove path does not exist", op.path));
    }
    return {};
}

auto transaction_child(am::Transaction& transaction, const am::ObjId& parent, std::string_view key)
    -> std::expected<std::optional<am::ObjId>, am::Error> {
    return transaction.get_obj_id(parent, key);
}

auto ensure_parent(am::Transaction& transaction,
                   am::ObjId current,
                   const std::vector<std::string>& components)
    -> std::expected<am::ObjId, am::Error> {
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        auto next = transaction_child(transaction, current, components[i]);
        if (!next)
            return std::unexpected(next.error());
        if (*next) {
            current = **next;
        } else {
            auto created = transaction.put_object(current, components[i], am::ObjType::map);
            if (!created)
                return std::unexpected(created.error());
            current = *created;
        }
    }
    return current;
}

auto clone_object(am::Transaction& transaction,
                  const am::ObjId& source,
                  const am::ObjId& destination) -> std::expected<void, am::Error> {
    auto keys = transaction.keys(source);
    if (!keys)
        return std::unexpected(keys.error());
    for (const auto& key : *keys) {
        auto nested = transaction.get_obj_id(source, key);
        if (!nested)
            return std::unexpected(nested.error());
        if (*nested) {
            auto created = transaction.put_object(destination, key, am::ObjType::map);
            if (!created)
                return std::unexpected(created.error());
            auto cloned = clone_object(transaction, **nested, *created);
            if (!cloned)
                return cloned;
            continue;
        }
        auto value = transaction.get(source, key);
        if (!value)
            return std::unexpected(value.error());
        if (!*value)
            continue;
        const auto* scalar = std::get_if<am::ScalarValue>(&**value);
        if (!scalar)
            return std::unexpected(am::Error{am::ErrorKind::invalid_value_type,
                                             "child object was not returned as an object"});
        auto inserted = transaction.put(destination, key, *scalar);
        if (!inserted)
            return inserted;
    }
    return {};
}

auto append_operation(am::Transaction& transaction, const operation& op, const actor_id& actor)
    -> std::expected<void, am::Error> {
    auto list = transaction.get_obj_id(am::root, ops_key);
    if (!list)
        return std::unexpected(list.error());
    if (!*list)
        return std::unexpected(
            am::Error{am::ErrorKind::invalid_obj_id, "operation log is missing"});
    auto length = transaction.length(**list);
    if (!length)
        return std::unexpected(length.error());
    auto item = transaction.insert_object(**list, *length, am::ObjType::map);
    if (!item)
        return std::unexpected(item.error());
    for (auto result : {transaction.put(*item, id_key, bytes(op.id)),
                        transaction.put(*item, kind_key, static_cast<std::uint64_t>(op.kind)),
                        transaction.put(*item, path_key, op.path),
                        transaction.put(*item, to_key, op.to),
                        transaction.put(*item, ref_key, op.ref),
                        transaction.put(*item, mode_key, static_cast<std::uint64_t>(op.mode)),
                        transaction.put(*item, "entry_id", bytes(op.entry_id)),
                        transaction.put(*item, by_key, bytes(actor))}) {
        if (!result)
            return result;
    }
    return {};
}

auto apply_write(am::Transaction& transaction, const am::ObjId& tree_root, const operation& op)
    -> std::expected<void, am::Error> {
    const auto components = *split_path(op.path);
    auto parent = ensure_parent(transaction, tree_root, components);
    if (!parent)
        return std::unexpected(parent.error());
    auto existing = transaction_child(transaction, *parent, components.back());
    if (!existing)
        return std::unexpected(existing.error());
    auto id = op.entry_id;
    if (*existing) {
        auto old = transaction.get(**existing, id_key);
        if (!old)
            return std::unexpected(old.error());
        if (*old) {
            auto stored = am::get_scalar<am::Bytes>(**old);
            if (stored && stored->size() == id.size())
                std::ranges::copy(*stored, id.begin());
        }
        auto reference = transaction.put(**existing, ref_key, op.ref);
        if (!reference)
            return reference;
        auto kind = transaction.put(**existing, kind_key, op.value_kind);
        if (!kind)
            return kind;
        return transaction.put(**existing, mode_key, static_cast<std::uint64_t>(op.mode));
    } else {
        auto created = transaction.put_object(*parent, components.back(), am::ObjType::map);
        if (!created)
            return std::unexpected(created.error());
        for (auto result :
             {transaction.put(*created, kind_key, op.value_kind),
              transaction.put(*created, id_key, bytes(id)),
              transaction.put(*created, mode_key, static_cast<std::uint64_t>(op.mode)),
              transaction.put(*created, ref_key, op.ref)}) {
            if (!result)
                return result;
        }
    }
    return {};
}

auto transaction_find(am::Transaction& transaction,
                      const am::ObjId& tree_root,
                      const std::vector<std::string>& components)
    -> std::expected<std::optional<am::ObjId>, am::Error> {
    auto current = std::optional<am::ObjId>{tree_root};
    for (const auto& component : components) {
        auto next = transaction_child(transaction, *current, component);
        if (!next)
            return std::unexpected(next.error());
        if (!*next)
            return std::optional<am::ObjId>{};
        current = **next;
    }
    return current;
}

auto apply_move(am::Transaction& transaction, const am::ObjId& tree_root, const operation& op)
    -> std::expected<void, am::Error> {
    const auto source_parts = *split_path(op.path);
    const auto destination_parts = *split_path(op.to);
    auto source_parent_parts =
        std::vector<std::string>{source_parts.begin(), source_parts.end() - 1};
    auto destination_parent_parts =
        std::vector<std::string>{destination_parts.begin(), destination_parts.end() - 1};
    auto source_parent = transaction_find(transaction, tree_root, source_parent_parts);
    auto destination_parent = transaction_find(transaction, tree_root, destination_parent_parts);
    if (!source_parent)
        return std::unexpected(source_parent.error());
    if (!destination_parent)
        return std::unexpected(destination_parent.error());
    if (!*source_parent || !*destination_parent) {
        return std::unexpected(am::Error{am::ErrorKind::invalid_obj_id, "move parent is missing"});
    }
    auto source = transaction_child(transaction, **source_parent, source_parts.back());
    if (!source)
        return std::unexpected(source.error());
    if (!*source) {
        return std::unexpected(am::Error{am::ErrorKind::invalid_obj_id, "move source is missing"});
    }
    auto destination =
        transaction.put_object(**destination_parent, destination_parts.back(), am::ObjType::map);
    if (!destination)
        return std::unexpected(destination.error());
    auto cloned = clone_object(transaction, **source, *destination);
    if (!cloned)
        return cloned;
    return transaction.delete_key(**source_parent, source_parts.back());
}

auto apply_remove(am::Transaction& transaction, const am::ObjId& tree_root, const operation& op)
    -> std::expected<void, am::Error> {
    const auto components = *split_path(op.path);
    auto parent_parts = std::vector<std::string>{components.begin(), components.end() - 1};
    auto parent = transaction_find(transaction, tree_root, parent_parts);
    if (!parent)
        return std::unexpected(parent.error());
    if (!*parent) {
        return std::unexpected(
            am::Error{am::ErrorKind::invalid_obj_id, "remove parent is missing"});
    }
    return transaction.delete_key(**parent, components.back());
}

} // namespace

auto schema::initialize(am::Document& document, const repo_id& repository) -> result<void> {
    try {
        auto initialized =
            document.transact([&](am::Transaction& transaction) -> std::expected<void, am::Error> {
                for (auto result : {
                         transaction.put(
                             am::root, repo_key, am::Bytes{repository.begin(), repository.end()}),
                         transaction.put(am::root, format_key, std::uint64_t{1}),
                     }) {
                    if (!result)
                        return result;
                }
                for (const auto& [key, type] : {
                         std::pair{tree_key, am::ObjType::map},
                         std::pair{refs_key, am::ObjType::map},
                         std::pair{locks_key, am::ObjType::map},
                         std::pair{nested_key, am::ObjType::map},
                         std::pair{config_key, am::ObjType::map},
                         std::pair{ops_key, am::ObjType::list},
                     }) {
                    auto created = transaction.put_object(am::root, key, type);
                    if (!created)
                        return std::unexpected(created.error());
                }
                return {};
            });
        if (!initialized)
            return std::unexpected(
                automerge_error(initialized.error(), errc::corrupt, "initialize CRDT schema"));
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(error{errc::internal, exception.what(), "initialize CRDT schema"});
    }
}

auto schema::validate(const am::Document& document, const std::vector<operation>& operations)
    -> result<void> {
    auto root_valid = valid_root(document);
    if (!root_valid)
        return std::unexpected(root_valid.error());
    if (!*root_valid)
        return std::unexpected(corrupt("invalid repository document root"));
    if (operations.empty())
        return std::unexpected(invalid("transaction has no operations"));
    for (const auto& op : operations) {
        result<void> checked;
        switch (op.kind) {
        case operation_kind::write:
            checked = validate_write(document, op);
            break;
        case operation_kind::move:
            checked = validate_move(document, op);
            break;
        case operation_kind::remove:
            checked = validate_remove(document, op);
            break;
        }
        if (!checked)
            return checked;
    }
    return {};
}

auto schema::apply(am::Document& document,
                   const std::vector<operation>& operations,
                   const actor_id& actor,
                   std::string_view message,
                   std::int64_t time) -> result<void> {
    try {
        auto tree_root = tree(document);
        if (!tree_root)
            return std::unexpected(tree_root.error());
        if (!*tree_root)
            return std::unexpected(corrupt("tree root is missing"));
        auto applied = document.transact(
            [&](am::Transaction& transaction) -> std::expected<void, am::Error> {
                for (const auto& op : operations) {
                    auto result = std::expected<void, am::Error>{};
                    switch (op.kind) {
                    case operation_kind::write:
                        result = apply_write(transaction, **tree_root, op);
                        break;
                    case operation_kind::move:
                        result = apply_move(transaction, **tree_root, op);
                        break;
                    case operation_kind::remove:
                        result = apply_remove(transaction, **tree_root, op);
                        break;
                    }
                    if (!result)
                        return result;
                    result = append_operation(transaction, op, actor);
                    if (!result)
                        return result;
                }
                return {};
            },
            am::CommitOptions{.message = std::string{message}, .time = time});
        if (!applied)
            return std::unexpected(
                automerge_error(applied.error(), errc::invalid_argument, "apply CRDT transaction"));
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(error{errc::internal, exception.what(), "apply CRDT transaction"});
    }
}

auto schema::inspect(const am::Document& document, std::string_view path)
    -> result<std::optional<entry>> {
    auto components = split_path(path);
    if (!components)
        return std::unexpected(components.error());
    auto object = find_object(document, *components);
    if (!object)
        return std::unexpected(object.error());
    if (!*object)
        return std::optional<entry>{};
    auto entry_object = is_entry(document, **object);
    if (!entry_object)
        return std::unexpected(entry_object.error());
    if (!*entry_object)
        return std::optional<entry>{};
    auto kind = checked(
        document.get<std::string>(**object, kind_key), errc::corrupt, "read tree entry kind");
    auto stored_id =
        checked(document.get<am::Bytes>(**object, id_key), errc::corrupt, "read tree entry id");
    auto mode = checked(
        document.get<std::uint64_t>(**object, mode_key), errc::corrupt, "read tree entry mode");
    auto ref = checked(
        document.get<std::string>(**object, ref_key), errc::corrupt, "read tree entry reference");
    if (!kind)
        return std::unexpected(kind.error());
    if (!stored_id)
        return std::unexpected(stored_id.error());
    if (!mode)
        return std::unexpected(mode.error());
    if (!ref)
        return std::unexpected(ref.error());
    auto id = fixed_id(*stored_id, id_key);
    if (!*kind || !id || !*mode || **mode > std::numeric_limits<std::uint32_t>::max() || !*ref) {
        return std::unexpected(corrupt("invalid tree entry", std::string{path}));
    }
    return std::optional<entry>{
        entry{std::move(**kind), *id, static_cast<std::uint32_t>(**mode), std::move(**ref)}};
}

auto schema::entry_names(const am::Document& document) -> result<std::vector<std::string>> {
    auto root = tree(document);
    if (!root)
        return std::unexpected(root.error());
    if (!*root)
        return std::unexpected(corrupt("tree root is missing"));
    auto keys = checked(document.keys(**root), errc::corrupt, "read tree entries");
    if (!keys)
        return std::unexpected(keys.error());
    return *keys;
}

auto schema::entries(const am::Document& document) -> result<std::vector<entry>> {
    auto keys = entry_names(document);
    if (!keys)
        return std::unexpected(keys.error());
    auto output = std::vector<entry>{};
    for (const auto& name : *keys) {
        auto item = inspect(document, name);
        if (!item)
            return std::unexpected(item.error());
        if (*item)
            output.push_back(std::move(**item));
    }
    return output;
}

auto schema::operations(const am::Document& document) -> result<std::vector<operation>> {
    auto list =
        checked(document.get_obj_id(am::root, ops_key), errc::corrupt, "read operation log");
    if (!list)
        return std::unexpected(list.error());
    if (!*list)
        return std::unexpected(corrupt("operation log is missing"));
    const auto changes = document.get_changes();
    auto objects = std::vector<am::ObjId>{};
    for (const auto& change : changes) {
        for (const auto& op : change.operations) {
            if (op.obj == **list && op.action == am::OpType::insert &&
                std::get_if<am::ObjType>(&op.value) &&
                *std::get_if<am::ObjType>(&op.value) == am::ObjType::map) {
                objects.emplace_back(op.id);
            }
        }
    }
    auto object_indexes = std::unordered_map<am::ObjId, std::size_t>{};
    for (std::size_t i = 0; i < objects.size(); ++i)
        object_indexes.emplace(objects[i], i);
    auto output = std::vector<operation>{objects.size()};
    auto present = std::vector<std::uint8_t>(objects.size(), 0);
    for (const auto& change : changes) {
        for (const auto& op : change.operations) {
            const auto found = object_indexes.find(op.obj);
            if (found == object_indexes.end() || op.action != am::OpType::put)
                continue;
            const auto index = found->second;
            const auto* key = std::get_if<std::string>(&op.key);
            if (!key)
                continue;
            if (*key == id_key) {
                auto id = am::get_scalar<am::Bytes>(op.value);
                if (!id || id->size() != node_id{}.size())
                    return std::unexpected(corrupt("invalid operation id"));
                std::ranges::copy(*id, output[index].id.begin());
                present[index] |= 1U;
            } else if (*key == kind_key) {
                auto kind = am::get_scalar<std::uint64_t>(op.value);
                if (!kind || *kind < 1 || *kind > 3)
                    return std::unexpected(corrupt("invalid operation kind"));
                output[index].kind = static_cast<operation_kind>(*kind);
                present[index] |= 2U;
            } else if (*key == path_key) {
                auto value = am::get_scalar<std::string>(op.value);
                if (!value)
                    return std::unexpected(corrupt("invalid operation path"));
                output[index].path = std::move(*value);
                present[index] |= 4U;
            } else if (*key == to_key) {
                auto value = am::get_scalar<std::string>(op.value);
                if (!value)
                    return std::unexpected(corrupt("invalid operation destination"));
                output[index].to = std::move(*value);
                present[index] |= 8U;
            } else if (*key == ref_key) {
                auto value = am::get_scalar<std::string>(op.value);
                if (!value)
                    return std::unexpected(corrupt("invalid operation reference"));
                output[index].ref = std::move(*value);
                present[index] |= 16U;
            } else if (*key == mode_key) {
                auto value = am::get_scalar<std::uint64_t>(op.value);
                if (!value || *value > std::numeric_limits<std::uint32_t>::max())
                    return std::unexpected(corrupt("invalid operation mode"));
                output[index].mode = static_cast<std::uint32_t>(*value);
                present[index] |= 32U;
            } else if (*key == "entry_id") {
                auto id = am::get_scalar<am::Bytes>(op.value);
                if (!id || id->size() != node_id{}.size())
                    return std::unexpected(corrupt("invalid entry id"));
                std::ranges::copy(*id, output[index].entry_id.begin());
                present[index] |= 64U;
            }
        }
    }
    if (std::ranges::any_of(present, [](auto fields) { return fields != 0x7fU; })) {
        return std::unexpected(corrupt("incomplete operation record"));
    }
    return output;
}

auto schema::valid_root(const am::Document& document) -> result<bool> {
    auto repository = checked(
        document.get<am::Bytes>(am::root, repo_key), errc::corrupt, "validate repository id");
    auto format = checked(document.get<std::uint64_t>(am::root, format_key),
                          errc::corrupt,
                          "validate repository format");
    if (!repository)
        return std::unexpected(repository.error());
    if (!format)
        return std::unexpected(format.error());
    if (!*repository || (*repository)->size() != repo_id{}.size() || !*format || **format != 1)
        return false;
    for (auto key : {tree_key, refs_key, locks_key, nested_key, config_key, ops_key}) {
        auto object = checked(
            document.get_obj_id(am::root, key), errc::corrupt, "validate repository object");
        if (!object)
            return std::unexpected(object.error());
        if (!*object)
            return false;
    }
    return true;
}

} // namespace liquid::crdt
