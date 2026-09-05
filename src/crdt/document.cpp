#include "crdt/document.hpp"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>

#include <automerge-cpp/sync_state.hpp>

#include "liquid/hash.hpp"

namespace liquid::crdt {
namespace {

namespace am = automerge_cpp;

auto internal(std::string message, std::string context) -> error {
    return error{errc::internal, std::move(message), std::move(context)};
}

auto corrupt(std::string message, std::string context) -> error {
    return error{errc::corrupt, std::move(message), std::move(context)};
}

auto automerge_error(const am::Error& source, errc code, std::string context) -> error {
    auto message = source.message;
    if (message.empty())
        message = std::string{am::to_string_view(source.kind)};
    return error{code, std::move(message), std::move(context)};
}

auto key_for(const head_set& heads) -> std::string {
    auto sorted = heads;
    std::ranges::sort(sorted);
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto result = std::string{};
    result.reserve(sorted.size() * 64);
    for (const auto& current : sorted) {
        for (const auto byte : current) {
            const auto value = std::to_integer<unsigned char>(byte);
            result.push_back(digits[value >> 4U]);
            result.push_back(digits[value & 0x0fU]);
        }
    }
    return result;
}

auto public_heads(const std::vector<am::ChangeHash>& heads) -> head_set {
    auto output = head_set{};
    output.reserve(heads.size());
    std::ranges::transform(
        heads, std::back_inserter(output), [](const auto& value) { return value.bytes; });
    std::ranges::sort(output);
    return output;
}

auto make_random_id() -> node_id {
    auto source = std::random_device{};
    auto output = node_id{};
    std::ranges::generate(output, [&] { return std::byte{static_cast<unsigned char>(source())}; });
    return output;
}

auto zero(const node_id& id) -> bool {
    return std::ranges::all_of(id, [](std::byte byte) { return byte == std::byte{0}; });
}

auto operation_message(const std::vector<operation>& operations) -> std::string {
    if (operations.size() != 1)
        return "drip";
    switch (operations.front().kind) {
    case operation_kind::write:
        return "write";
    case operation_kind::move:
        return "move";
    case operation_kind::remove:
        return "remove";
    }
    return "drip";
}

} // namespace

struct sync_state::impl {
    am::SyncState value;
};

struct document::impl {
    am::Document value;
    actor_id actor{};
    mutable std::map<std::string, std::set<node_id>> operation_snapshots;

    impl(am::Document input, actor_id identity) : value(std::move(input)), actor(identity) {
        value.set_read_locking(false);
    }
};

sync_state::sync_state() : impl_(std::make_unique<impl>()) {}
sync_state::~sync_state() = default;
sync_state::sync_state(sync_state&&) noexcept = default;
auto sync_state::operator=(sync_state&&) noexcept -> sync_state& = default;

document::document(std::unique_ptr<impl> implementation) : impl_(std::move(implementation)) {}
document::document(document&&) noexcept = default;
auto document::operator=(document&&) noexcept -> document& = default;
document::~document() = default;

auto derive_actor_id(std::string_view taap_id, std::string_view device_id) -> result<actor_id> {
    auto input = std::vector<std::byte>{};
    input.reserve(taap_id.size() + device_id.size());
    input.insert(input.end(),
                 reinterpret_cast<const std::byte*>(taap_id.data()),
                 reinterpret_cast<const std::byte*>(taap_id.data() + taap_id.size()));
    input.insert(input.end(),
                 reinterpret_cast<const std::byte*>(device_id.data()),
                 reinterpret_cast<const std::byte*>(device_id.data() + device_id.size()));
    auto digest = liquid::hash(algorithm::blake3, input);
    if (!digest)
        return std::unexpected(digest.error());
    auto output = actor_id{};
    std::ranges::copy_n(digest->bytes.begin(), output.size(), output.begin());
    return output;
}

auto document::create(const repo_id& repository,
                      std::string_view taap_id,
                      std::string_view device_id) -> result<document> {
    try {
        auto actor = derive_actor_id(taap_id, device_id);
        if (!actor)
            return std::unexpected(actor.error());
        auto value = am::Document{1u};
        value.set_actor_id(am::ActorId{*actor});
        value.set_read_locking(false);
        if (auto initialized = schema::initialize(value, repository); !initialized)
            return std::unexpected(initialized.error());
        return document{std::make_unique<impl>(std::move(value), *actor)};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "create CRDT document"));
    }
}

auto document::create_sync_receiver(std::string_view taap_id, std::string_view device_id)
    -> result<document> {
    try {
        auto actor = derive_actor_id(taap_id, device_id);
        if (!actor) {
            return std::unexpected(actor.error());
        }
        auto value = am::Document{1u};
        value.set_actor_id(am::ActorId{*actor});
        value.set_read_locking(false);
        return document{std::make_unique<impl>(std::move(value), *actor)};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "create CRDT sync receiver"));
    }
}

auto document::load(std::span<const std::byte> bytes,
                    std::string_view taap_id,
                    std::string_view device_id) -> result<document> {
    try {
        auto actor = derive_actor_id(taap_id, device_id);
        if (!actor)
            return std::unexpected(actor.error());
        auto loaded = am::Document::load(bytes);
        if (!loaded)
            return std::unexpected(
                automerge_error(loaded.error(), errc::corrupt, "load CRDT document"));
        auto valid = schema::valid_root(*loaded);
        if (!valid)
            return std::unexpected(valid.error());
        if (!*valid)
            return std::unexpected(corrupt("invalid repository document", "load CRDT document"));
        loaded->set_actor_id(am::ActorId{*actor});
        return document{std::make_unique<impl>(std::move(*loaded), *actor)};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "load CRDT document"));
    }
}

auto document::load_shallow(std::span<const std::byte> bytes,
                            std::string_view taap_id,
                            std::string_view device_id) -> result<document> {
    try {
        auto actor = derive_actor_id(taap_id, device_id);
        if (!actor)
            return std::unexpected(actor.error());
        auto loaded = am::Document::load_shallow(bytes);
        if (!loaded)
            return std::unexpected(
                automerge_error(loaded.error(), errc::corrupt, "load shallow CRDT document"));
        auto valid = schema::valid_root(*loaded);
        if (!valid)
            return std::unexpected(valid.error());
        if (!*valid) {
            return std::unexpected(
                corrupt("invalid repository document", "load shallow CRDT document"));
        }
        loaded->set_actor_id(am::ActorId{*actor});
        return document{std::make_unique<impl>(std::move(*loaded), *actor)};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "load shallow CRDT document"));
    }
}

auto document::actor() const -> actor_id {
    return impl_->actor;
}

auto document::save() const -> result<std::vector<std::byte>> {
    try {
        return impl_->value.save();
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "save CRDT document"));
    }
}

auto document::save_compact() const -> result<std::vector<std::byte>> {
    try {
        return impl_->value.save_compact();
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "save compact CRDT document"));
    }
}

auto document::heads() -> result<head_set> {
    try {
        auto result = public_heads(impl_->value.get_heads());
        auto operations = schema::operations(impl_->value);
        if (!operations)
            return std::unexpected(operations.error());
        auto ids = std::set<node_id>{};
        for (const auto& operation : *operations)
            ids.insert(operation.id);
        impl_->operation_snapshots[key_for(result)] = std::move(ids);
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "read CRDT heads"));
    }
}

auto document::transact(std::vector<operation> operations, commit_options options)
    -> result<std::vector<operation>> {
    try {
        if (options.message.empty())
            options.message = operation_message(operations);
        for (auto& operation : operations) {
            if (zero(operation.id))
                operation.id = make_random_id();
            if (operation.kind == operation_kind::write) {
                auto existing = schema::inspect(impl_->value, operation.path);
                if (!existing)
                    return std::unexpected(existing.error());
                if (*existing)
                    operation.entry_id = (*existing)->id;
                else if (zero(operation.entry_id))
                    operation.entry_id = make_random_id();
            }
        }
        if (auto valid = schema::validate(impl_->value, operations); !valid)
            return std::unexpected(valid.error());
        if (auto applied = schema::apply(
                impl_->value, operations, impl_->actor, options.message, options.time);
            !applied)
            return std::unexpected(applied.error());
        return operations;
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "CRDT transaction"));
    }
}

auto document::diff(const head_set& before_heads) const -> result<std::vector<operation>> {
    auto snapshot = impl_->operation_snapshots.find(key_for(before_heads));
    if (snapshot == impl_->operation_snapshots.end()) {
        return std::unexpected(
            error{errc::invalid_argument, "heads were not observed by this document", "CRDT diff"});
    }
    auto operations = schema::operations(impl_->value);
    if (!operations)
        return std::unexpected(operations.error());
    auto output = std::vector<operation>{};
    std::ranges::copy_if(*operations, std::back_inserter(output), [&](const operation& op) {
        return !snapshot->second.contains(op.id);
    });
    return output;
}

auto document::inspect(std::string_view path) const -> result<std::optional<entry>> {
    return schema::inspect(impl_->value, path);
}

auto document::entries() const -> result<std::vector<entry>> {
    return schema::entries(impl_->value);
}
auto document::entry_names() const -> result<std::vector<std::string>> {
    return schema::entry_names(impl_->value);
}
auto document::operations() const -> result<std::vector<operation>> {
    return schema::operations(impl_->value);
}

auto document::generate_sync_message(sync_state& state) const
    -> result<std::optional<std::vector<std::byte>>> {
    try {
        auto message = impl_->value.generate_sync_message(state.impl_->value);
        if (!message)
            return std::optional<std::vector<std::byte>>{};
        return std::optional<std::vector<std::byte>>{message->encode()};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "generate CRDT sync message"));
    }
}

auto document::receive_sync_message(sync_state& state, std::span<const std::byte> bytes)
    -> result<void> {
    try {
        auto decoded = am::SyncMessage::decode(bytes);
        if (!decoded)
            return std::unexpected(
                automerge_error(decoded.error(), errc::corrupt, "decode CRDT sync message"));
        auto candidate = impl_->value;
        auto candidate_state = state.impl_->value;
        auto patches = candidate.receive_sync_message_with_patches(candidate_state, *decoded);
        if (!patches)
            return std::unexpected(
                automerge_error(patches.error(), errc::corrupt, "receive CRDT sync message"));
        auto valid = schema::valid_root(candidate);
        if (!valid)
            return std::unexpected(valid.error());
        if (!*valid)
            return std::unexpected(corrupt("sync produced an invalid document", "receive sync"));
        impl_->value = std::move(candidate);
        state.impl_->value = std::move(candidate_state);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "receive CRDT sync message"));
    }
}

auto document::merge(const document& other) -> result<void> {
    try {
        auto candidate = impl_->value;
        auto patches = candidate.merge_with_patches(other.impl_->value);
        if (!patches)
            return std::unexpected(
                automerge_error(patches.error(), errc::corrupt, "merge CRDT documents"));
        auto valid = schema::valid_root(candidate);
        if (!valid)
            return std::unexpected(valid.error());
        if (!*valid)
            return std::unexpected(corrupt("merge produced an invalid document", "CRDT merge"));
        impl_->value = std::move(candidate);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(internal(exception.what(), "merge CRDT documents"));
    }
}

directory_document::directory_document(document value) : document_(std::move(value)) {}
directory_document::directory_document(directory_document&&) noexcept = default;
auto directory_document::operator=(directory_document&&) noexcept -> directory_document& = default;
directory_document::~directory_document() = default;

auto directory_document::create(const repo_id& repository,
                                std::string_view taap_id,
                                std::string_view device_id) -> result<directory_document> {
    auto value = document::create(repository, taap_id, device_id);
    if (!value)
        return std::unexpected(value.error());
    return directory_document{std::move(*value)};
}

auto directory_document::create_sync_receiver(std::string_view taap_id, std::string_view device_id)
    -> result<directory_document> {
    auto value = document::create_sync_receiver(taap_id, device_id);
    if (!value) {
        return std::unexpected(value.error());
    }
    return directory_document{std::move(*value)};
}

auto directory_document::load(std::span<const std::byte> bytes,
                              std::string_view taap_id,
                              std::string_view device_id) -> result<directory_document> {
    auto value = document::load_shallow(bytes, taap_id, device_id);
    if (!value)
        return std::unexpected(value.error());
    return directory_document{std::move(*value)};
}

auto directory_document::load_sync(std::span<const std::byte> bytes,
                                   std::string_view taap_id,
                                   std::string_view device_id) -> result<directory_document> {
    auto value = document::load(bytes, taap_id, device_id);
    if (!value) {
        return std::unexpected(value.error());
    }
    return directory_document{std::move(*value)};
}

auto directory_document::entries() const -> result<std::vector<named_entry>> {
    auto names = document_.entry_names();
    if (!names)
        return std::unexpected(names.error());
    auto output = std::vector<named_entry>{};
    for (auto& name : *names) {
        auto value = document_.inspect(name);
        if (!value)
            return std::unexpected(value.error());
        if (*value)
            output.push_back({std::move(name), std::move(**value)});
    }
    return output;
}

auto directory_document::entry_at(std::string_view name) const -> result<std::optional<entry>> {
    if (name.empty() || name.contains('/'))
        return std::unexpected(
            error{errc::invalid_argument, "directory entry name is invalid", std::string{name}});
    return document_.inspect(name);
}

auto directory_document::put_entry(std::string name, entry value, commit_options options)
    -> result<void> {
    if (name.empty() || name.contains('/') || value.ref.empty())
        return std::unexpected(error{errc::invalid_argument, "directory entry is invalid", name});
    auto applied = document_.transact({operation{.kind = operation_kind::write,
                                                 .value_kind = std::move(value.kind),
                                                 .path = std::move(name),
                                                 .ref = std::move(value.ref),
                                                 .mode = value.mode,
                                                 .entry_id = value.id}},
                                      std::move(options));
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}

auto directory_document::remove_entry(std::string name, commit_options options) -> result<void> {
    if (name.empty() || name.contains('/'))
        return std::unexpected(
            error{errc::invalid_argument, "directory entry name is invalid", name});
    auto applied = document_.transact(
        {operation{.kind = operation_kind::remove, .path = std::move(name)}}, std::move(options));
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}

auto directory_document::rename_entry(std::string from, std::string to, commit_options options)
    -> result<void> {
    if (from.empty() || to.empty() || from.contains('/') || to.contains('/'))
        return std::unexpected(error{errc::invalid_argument, "directory entry name is invalid"});
    auto applied = document_.transact(
        {operation{.kind = operation_kind::move, .path = std::move(from), .to = std::move(to)}},
        std::move(options));
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}

auto directory_document::save() const -> result<std::vector<std::byte>> {
    return document_.save_compact();
}
auto directory_document::save_full() const -> result<std::vector<std::byte>> {
    return document_.save();
}
auto directory_document::save_sync() const -> result<std::vector<std::byte>> {
    return document_.save_compact();
}
auto directory_document::heads() -> result<head_set> {
    return document_.heads();
}
auto directory_document::generate_sync_message(sync_state& state) const
    -> result<std::optional<std::vector<std::byte>>> {
    return document_.generate_sync_message(state);
}
auto directory_document::receive_sync_message(sync_state& state, std::span<const std::byte> bytes)
    -> result<void> {
    return document_.receive_sync_message(state, bytes);
}
auto directory_document::merge(const directory_document& other) -> result<void> {
    return document_.merge(other.document_);
}

} // namespace liquid::crdt
