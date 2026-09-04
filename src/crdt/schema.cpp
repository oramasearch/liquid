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

auto bytes(const node_id &id) -> am::Bytes { return am::Bytes{id.begin(), id.end()}; }

auto fixed_id(const std::optional<am::Bytes> &input, std::string_view field) -> result<node_id> {
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

auto tree(const am::Document &document) -> std::optional<am::ObjId> {
    return document.get_obj_id(am::root, tree_key);
}

auto child(const am::Document &document, const am::ObjId &parent, std::string_view key)
    -> std::optional<am::ObjId> {
    return document.get_obj_id(parent, key);
}

auto is_entry(const am::Document &document, const am::ObjId &object) -> bool {
    return document.get<std::string>(object, kind_key).has_value();
}

auto find_object(const am::Document &document, const std::vector<std::string> &components)
    -> std::optional<am::ObjId> {
    auto current = tree(document);
    if (!current)
        return std::nullopt;
    for (const auto &component : components) {
        current = child(document, *current, component);
        if (!current)
            return std::nullopt;
    }
    return current;
}

auto find_parent(const am::Document &document, const std::vector<std::string> &components)
    -> std::optional<am::ObjId> {
    if (components.empty())
        return std::nullopt;
    return find_object(document,
                       std::vector<std::string>{components.begin(), components.end() - 1});
}

auto validate_write(const am::Document &document, const operation &op) -> result<void> {
    auto components = split_path(op.path);
    if (!components)
        return std::unexpected(components.error());
    if (op.ref.empty())
        return std::unexpected(invalid("write reference is empty", op.path));
    auto current = tree(document);
    if (!current)
        return std::unexpected(corrupt("tree root is missing"));
    for (std::size_t i = 0; i + 1 < components->size(); ++i) {
        auto next = child(document, *current, (*components)[i]);
        if (!next)
            break;
        if (is_entry(document, *next)) {
            return std::unexpected(invalid("path component is not a directory", op.path));
        }
        current = next;
    }
    if (auto existing = find_object(document, *components);
        existing && !is_entry(document, *existing)) {
        return std::unexpected(invalid("cannot overwrite a directory", op.path));
    }
    return {};
}

auto validate_move(const am::Document &document, const operation &op) -> result<void> {
    auto source = split_path(op.path);
    if (!source)
        return std::unexpected(source.error());
    auto destination = split_path(op.to);
    if (!destination)
        return std::unexpected(destination.error());
    if (op.path == op.to || op.to.starts_with(op.path + "/")) {
        return std::unexpected(invalid("move destination is inside its source", op.to));
    }
    if (!find_object(document, *source)) {
        return std::unexpected(invalid("move source does not exist", op.path));
    }
    if (find_object(document, *destination)) {
        return std::unexpected(invalid("move destination already exists", op.to));
    }
    if (!find_parent(document, *destination)) {
        return std::unexpected(invalid("move destination parent does not exist", op.to));
    }
    return {};
}

auto validate_remove(const am::Document &document, const operation &op) -> result<void> {
    auto components = split_path(op.path);
    if (!components)
        return std::unexpected(components.error());
    if (!find_object(document, *components)) {
        return std::unexpected(invalid("remove path does not exist", op.path));
    }
    return {};
}

auto ensure_parent(am::Document &document, am::Transaction &transaction,
                   const std::vector<std::string> &components) -> am::ObjId {
    auto current = *tree(document);
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        if (auto next = child(document, current, components[i])) {
            current = *next;
        } else {
            current = transaction.put_object(current, components[i], am::ObjType::map);
        }
    }
    return current;
}

void clone_object(const am::Document &document, am::Transaction &transaction,
                  const am::ObjId &source, const am::ObjId &destination) {
    for (const auto &key : document.keys(source)) {
        if (auto nested = document.get_obj_id(source, key)) {
            const auto created = transaction.put_object(destination, key, am::ObjType::map);
            clone_object(document, transaction, *nested, created);
        } else if (auto text = document.get<std::string>(source, key)) {
            transaction.put(destination, key, *text);
        } else if (auto raw = document.get<am::Bytes>(source, key)) {
            transaction.put(destination, key, *raw);
        } else if (auto number = document.get<std::uint64_t>(source, key)) {
            transaction.put(destination, key, *number);
        }
    }
}

void append_operation(const am::Document &document, am::Transaction &transaction,
                      const operation &op, const actor_id &actor) {
    const auto list = *document.get_obj_id(am::root, ops_key);
    const auto item = transaction.insert_object(list, document.length(list), am::ObjType::map);
    transaction.put(item, id_key, bytes(op.id));
    transaction.put(item, kind_key, static_cast<std::uint64_t>(op.kind));
    transaction.put(item, path_key, op.path);
    transaction.put(item, to_key, op.to);
    transaction.put(item, ref_key, op.ref);
    transaction.put(item, mode_key, static_cast<std::uint64_t>(op.mode));
    transaction.put(item, "entry_id", bytes(op.entry_id));
    transaction.put(item, by_key, bytes(actor));
}

void apply_write(am::Document &document, am::Transaction &transaction, const operation &op) {
    const auto components = *split_path(op.path);
    const auto parent = ensure_parent(document, transaction, components);
    auto existing = child(document, parent, components.back());
    auto id = op.entry_id;
    if (existing) {
        if (auto old = document.get<am::Bytes>(*existing, id_key);
            old && old->size() == id.size()) {
            std::ranges::copy(*old, id.begin());
        }
        transaction.put(*existing, ref_key, op.ref);
        transaction.put(*existing, mode_key, static_cast<std::uint64_t>(op.mode));
    } else {
        const auto created = transaction.put_object(parent, components.back(), am::ObjType::map);
        transaction.put(created, kind_key, "blob");
        transaction.put(created, id_key, bytes(id));
        transaction.put(created, mode_key, static_cast<std::uint64_t>(op.mode));
        transaction.put(created, ref_key, op.ref);
    }
}

void apply_move(am::Document &document, am::Transaction &transaction, const operation &op) {
    const auto source_parts = *split_path(op.path);
    const auto destination_parts = *split_path(op.to);
    const auto source_parent = *find_parent(document, source_parts);
    const auto destination_parent = *find_parent(document, destination_parts);
    const auto source = *child(document, source_parent, source_parts.back());
    const auto destination =
        transaction.put_object(destination_parent, destination_parts.back(), am::ObjType::map);
    clone_object(document, transaction, source, destination);
    transaction.delete_key(source_parent, source_parts.back());
}

void apply_remove(am::Document &document, am::Transaction &transaction, const operation &op) {
    const auto components = *split_path(op.path);
    transaction.delete_key(*find_parent(document, components), components.back());
}

} // namespace

auto schema::initialize(am::Document &document, const repo_id &repository) -> result<void> {
    try {
        document.transact([&](am::Transaction &transaction) {
            transaction.put(am::root, repo_key, am::Bytes{repository.begin(), repository.end()});
            transaction.put(am::root, format_key, std::uint64_t{1});
            transaction.put_object(am::root, tree_key, am::ObjType::map);
            transaction.put_object(am::root, refs_key, am::ObjType::map);
            transaction.put_object(am::root, locks_key, am::ObjType::map);
            transaction.put_object(am::root, nested_key, am::ObjType::map);
            transaction.put_object(am::root, config_key, am::ObjType::map);
            transaction.put_object(am::root, ops_key, am::ObjType::list);
        });
        return {};
    } catch (const std::exception &exception) {
        return std::unexpected(error{errc::internal, exception.what(), "initialize CRDT schema"});
    }
}

auto schema::validate(const am::Document &document, const std::vector<operation> &operations)
    -> result<void> {
    if (!valid_root(document))
        return std::unexpected(corrupt("invalid repository document root"));
    if (operations.empty())
        return std::unexpected(invalid("transaction has no operations"));
    for (const auto &op : operations) {
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

auto schema::apply(am::Document &document, const std::vector<operation> &operations,
                   const actor_id &actor) -> result<void> {
    try {
        document.transact([&](am::Transaction &transaction) {
            for (const auto &op : operations) {
                switch (op.kind) {
                case operation_kind::write:
                    apply_write(document, transaction, op);
                    break;
                case operation_kind::move:
                    apply_move(document, transaction, op);
                    break;
                case operation_kind::remove:
                    apply_remove(document, transaction, op);
                    break;
                }
                append_operation(document, transaction, op, actor);
            }
        });
        return {};
    } catch (const std::exception &exception) {
        return std::unexpected(error{errc::internal, exception.what(), "apply CRDT transaction"});
    }
}

auto schema::inspect(const am::Document &document, std::string_view path)
    -> result<std::optional<entry>> {
    auto components = split_path(path);
    if (!components)
        return std::unexpected(components.error());
    auto object = find_object(document, *components);
    if (!object || !is_entry(document, *object))
        return std::optional<entry>{};
    auto kind = document.get<std::string>(*object, kind_key);
    auto id = fixed_id(document.get<am::Bytes>(*object, id_key), id_key);
    auto mode = document.get<std::uint64_t>(*object, mode_key);
    auto ref = document.get<std::string>(*object, ref_key);
    if (!kind || !id || !mode || *mode > std::numeric_limits<std::uint32_t>::max() || !ref) {
        return std::unexpected(corrupt("invalid tree entry", std::string{path}));
    }
    return std::optional<entry>{
        entry{std::move(*kind), *id, static_cast<std::uint32_t>(*mode), std::move(*ref)}};
}

auto schema::operations(const am::Document &document) -> result<std::vector<operation>> {
    auto list = document.get_obj_id(am::root, ops_key);
    if (!list)
        return std::unexpected(corrupt("operation log is missing"));
    const auto changes = document.get_changes();
    auto objects = std::vector<am::ObjId>{};
    for (const auto &change : changes) {
        for (const auto &op : change.operations) {
            if (op.obj == *list && op.action == am::OpType::insert &&
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
    for (const auto &change : changes) {
        for (const auto &op : change.operations) {
            const auto found = object_indexes.find(op.obj);
            if (found == object_indexes.end() || op.action != am::OpType::put)
                continue;
            const auto index = found->second;
            const auto *key = std::get_if<std::string>(&op.key);
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

auto schema::valid_root(const am::Document &document) -> bool {
    const auto repository = document.get<am::Bytes>(am::root, repo_key);
    const auto format = document.get<std::uint64_t>(am::root, format_key);
    return repository && repository->size() == repo_id{}.size() && format && *format == 1 &&
           document.get_obj_id(am::root, tree_key) && document.get_obj_id(am::root, refs_key) &&
           document.get_obj_id(am::root, locks_key) && document.get_obj_id(am::root, nested_key) &&
           document.get_obj_id(am::root, config_key) && document.get_obj_id(am::root, ops_key);
}

} // namespace liquid::crdt
