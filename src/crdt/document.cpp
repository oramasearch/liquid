#include "crdt/document.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>

#include <automerge-cpp/change.hpp>
#include <automerge-cpp/op.hpp>
#include <automerge-cpp/sync_state.hpp>

#include "encoding/cbor.hpp"
#include "liquid/hash.hpp"

namespace liquid::crdt {
namespace {

namespace am = automerge_cpp;
namespace cbor = liquid::encoding::cbor;

auto internal(std::string message, std::string context) -> error {
    return error{errc::internal, std::move(message), std::move(context)};
}

auto corrupt(std::string message, std::string context) -> error {
    return error{errc::corrupt, std::move(message), std::move(context)};
}

template <std::size_t Size>
auto byte_vector(const std::array<std::byte, Size> &value) -> std::vector<std::byte> {
    return {value.begin(), value.end()};
}

template <std::size_t Size>
auto fixed_bytes(const cbor::value &value, std::string_view context)
    -> result<std::array<std::byte, Size>> {
    const auto *bytes = std::get_if<cbor::value::bytes>(&value.data);
    if (!bytes || bytes->size() != Size) {
        return std::unexpected(corrupt("invalid fixed-width byte string", std::string{context}));
    }
    auto output = std::array<std::byte, Size>{};
    std::ranges::copy(*bytes, output.begin());
    return output;
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
    return cbor::value{cbor::value::map{values}};
}

auto array(std::initializer_list<cbor::value> values) -> cbor::value {
    return cbor::value{cbor::value::array{values}};
}

auto field(const cbor::value &source, std::string_view name) -> result<const cbor::value *> {
    const auto *object = std::get_if<cbor::value::map>(&source.data);
    if (!object)
        return std::unexpected(corrupt("expected map", std::string{name}));
    for (const auto &[key, value] : *object) {
        const auto *text = std::get_if<std::string>(&key.data);
        if (text && *text == name)
            return &value;
    }
    return std::unexpected(corrupt("missing field", std::string{name}));
}

auto unsigned_field(const cbor::value &source, std::string_view name) -> result<std::uint64_t> {
    auto item = field(source, name);
    if (!item)
        return std::unexpected(item.error());
    const auto *value = std::get_if<std::uint64_t>(&(*item)->data);
    if (!value)
        return std::unexpected(corrupt("expected unsigned integer", std::string{name}));
    return *value;
}

auto signed_field(const cbor::value &source, std::string_view name) -> result<std::int64_t> {
    auto item = field(source, name);
    if (!item)
        return std::unexpected(item.error());
    if (const auto *value = std::get_if<std::int64_t>(&(*item)->data))
        return *value;
    if (const auto *value = std::get_if<std::uint64_t>(&(*item)->data);
        value && *value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*value);
    }
    return std::unexpected(corrupt("expected signed integer", std::string{name}));
}

auto array_field(const cbor::value &source, std::string_view name)
    -> result<const cbor::value::array *> {
    auto item = field(source, name);
    if (!item)
        return std::unexpected(item.error());
    const auto *value = std::get_if<cbor::value::array>(&(*item)->data);
    if (!value)
        return std::unexpected(corrupt("expected array", std::string{name}));
    return value;
}

auto encode_actor(const am::ActorId &actor) -> cbor::value {
    return cbor::value{byte_vector(actor.bytes)};
}

auto decode_actor(const cbor::value &value) -> result<am::ActorId> {
    auto bytes = fixed_bytes<am::ActorId::size>(value, "actor");
    if (!bytes)
        return std::unexpected(bytes.error());
    return am::ActorId{*bytes};
}

auto encode_hash(const am::ChangeHash &hash) -> cbor::value {
    return cbor::value{byte_vector(hash.bytes)};
}

auto decode_hash(const cbor::value &value) -> result<am::ChangeHash> {
    auto bytes = fixed_bytes<am::ChangeHash::size>(value, "change hash");
    if (!bytes)
        return std::unexpected(bytes.error());
    return am::ChangeHash{*bytes};
}

auto encode_hashes(const std::vector<am::ChangeHash> &hashes) -> cbor::value {
    auto output = cbor::value::array{};
    output.reserve(hashes.size());
    std::ranges::transform(hashes, std::back_inserter(output), encode_hash);
    return cbor::value{std::move(output)};
}

auto decode_hashes(const cbor::value &value) -> result<std::vector<am::ChangeHash>> {
    const auto *source = std::get_if<cbor::value::array>(&value.data);
    if (!source)
        return std::unexpected(corrupt("expected hash array", "sync frame"));
    auto output = std::vector<am::ChangeHash>{};
    output.reserve(source->size());
    for (const auto &item : *source) {
        auto hash = decode_hash(item);
        if (!hash)
            return std::unexpected(hash.error());
        output.push_back(*hash);
    }
    return output;
}

auto encode_op_id(const am::OpId &id) -> cbor::value {
    return array({cbor::value{id.counter}, encode_actor(id.actor)});
}

auto decode_op_id(const cbor::value &value) -> result<am::OpId> {
    const auto *source = std::get_if<cbor::value::array>(&value.data);
    if (!source || source->size() != 2) {
        return std::unexpected(corrupt("invalid operation identifier", "sync frame"));
    }
    const auto *counter = std::get_if<std::uint64_t>(&(*source)[0].data);
    auto actor = decode_actor((*source)[1]);
    if (!counter || !actor)
        return std::unexpected(corrupt("invalid operation identifier", "sync frame"));
    return am::OpId{*counter, *actor};
}

auto encode_obj_id(const am::ObjId &id) -> cbor::value {
    if (id.is_root())
        return cbor::value{cbor::null{}};
    return encode_op_id(std::get<am::OpId>(id.inner));
}

auto decode_obj_id(const cbor::value &value) -> result<am::ObjId> {
    if (std::holds_alternative<cbor::null>(value.data))
        return am::root;
    auto id = decode_op_id(value);
    if (!id)
        return std::unexpected(id.error());
    return am::ObjId{*id};
}

auto encode_value(const am::Value &value) -> result<cbor::value> {
    if (const auto *object = std::get_if<am::ObjType>(&value)) {
        return array(
            {cbor::value{std::uint64_t{9}}, cbor::value{static_cast<std::uint64_t>(*object)}});
    }
    const auto &scalar = std::get<am::ScalarValue>(value);
    return std::visit(
        [](const auto &item) -> result<cbor::value> {
            using type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<type, am::Null>) {
                return array({cbor::value{std::uint64_t{0}}, cbor::value{cbor::null{}}});
            } else if constexpr (std::is_same_v<type, bool>) {
                return array({cbor::value{std::uint64_t{1}}, cbor::value{item}});
            } else if constexpr (std::is_same_v<type, std::int64_t>) {
                return array({cbor::value{std::uint64_t{2}}, cbor::value{item}});
            } else if constexpr (std::is_same_v<type, std::uint64_t>) {
                return array({cbor::value{std::uint64_t{3}}, cbor::value{item}});
            } else if constexpr (std::is_same_v<type, double>) {
                const auto bits = std::bit_cast<std::uint64_t>(item);
                auto bytes = std::vector<std::byte>{};
                for (auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U})
                    bytes.push_back(std::byte{static_cast<unsigned char>(bits >> shift)});
                return array({cbor::value{std::uint64_t{4}}, cbor::value{std::move(bytes)}});
            } else if constexpr (std::is_same_v<type, am::Counter>) {
                return array({cbor::value{std::uint64_t{5}}, cbor::value{item.value}});
            } else if constexpr (std::is_same_v<type, am::Timestamp>) {
                return array({cbor::value{std::uint64_t{6}}, cbor::value{item.millis_since_epoch}});
            } else if constexpr (std::is_same_v<type, std::string>) {
                return array({cbor::value{std::uint64_t{7}}, cbor::value{item}});
            } else {
                return array({cbor::value{std::uint64_t{8}}, cbor::value{item}});
            }
        },
        scalar);
}

auto decode_value(const cbor::value &value) -> result<am::Value> {
    const auto *source = std::get_if<cbor::value::array>(&value.data);
    if (!source || source->size() != 2)
        return std::unexpected(corrupt("invalid value", "sync frame"));
    const auto *tag = std::get_if<std::uint64_t>(&(*source)[0].data);
    if (!tag)
        return std::unexpected(corrupt("invalid value tag", "sync frame"));
    const auto &payload = (*source)[1];
    switch (*tag) {
    case 0:
        if (!std::holds_alternative<cbor::null>(payload.data))
            break;
        return am::Value{am::ScalarValue{am::Null{}}};
    case 1:
        if (const auto *item = std::get_if<bool>(&payload.data))
            return am::Value{am::ScalarValue{*item}};
        break;
    case 2:
        if (const auto *item = std::get_if<std::int64_t>(&payload.data))
            return am::Value{am::ScalarValue{*item}};
        if (const auto *item = std::get_if<std::uint64_t>(&payload.data);
            item && *item <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return am::Value{am::ScalarValue{static_cast<std::int64_t>(*item)}};
        break;
    case 3:
        if (const auto *item = std::get_if<std::uint64_t>(&payload.data))
            return am::Value{am::ScalarValue{*item}};
        break;
    case 4: {
        auto item = fixed_bytes<sizeof(double)>(payload, "double");
        if (item) {
            auto bits = std::uint64_t{0};
            for (auto byte : *item)
                bits = (bits << 8U) | std::to_integer<unsigned char>(byte);
            return am::Value{am::ScalarValue{std::bit_cast<double>(bits)}};
        }
        break;
    }
    case 5: {
        if (const auto *item = std::get_if<std::int64_t>(&payload.data))
            return am::Value{am::ScalarValue{am::Counter{*item}}};
        if (const auto *item = std::get_if<std::uint64_t>(&payload.data);
            item && *item <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return am::Value{am::ScalarValue{am::Counter{static_cast<std::int64_t>(*item)}}};
        break;
    }
    case 6: {
        if (const auto *item = std::get_if<std::int64_t>(&payload.data))
            return am::Value{am::ScalarValue{am::Timestamp{*item}}};
        if (const auto *item = std::get_if<std::uint64_t>(&payload.data);
            item && *item <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return am::Value{am::ScalarValue{am::Timestamp{static_cast<std::int64_t>(*item)}}};
        break;
    }
    case 7:
        if (const auto *item = std::get_if<std::string>(&payload.data))
            return am::Value{am::ScalarValue{*item}};
        break;
    case 8:
        if (const auto *item = std::get_if<cbor::value::bytes>(&payload.data))
            return am::Value{am::ScalarValue{*item}};
        break;
    case 9:
        if (const auto *item = std::get_if<std::uint64_t>(&payload.data); item && *item <= 3)
            return am::Value{static_cast<am::ObjType>(*item)};
        break;
    }
    return std::unexpected(corrupt("invalid value payload", "sync frame"));
}

auto encode_op(const am::Op &op) -> result<cbor::value> {
    auto value = encode_value(op.value);
    if (!value)
        return std::unexpected(value.error());
    auto predecessors = cbor::value::array{};
    std::ranges::transform(op.pred, std::back_inserter(predecessors), encode_op_id);
    auto key = std::visit(
        [](const auto &item) -> cbor::value {
            using type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<type, std::string>)
                return cbor::value{item};
            else
                return cbor::value{static_cast<std::uint64_t>(item)};
        },
        op.key);
    auto after = op.insert_after ? encode_op_id(*op.insert_after) : cbor::value{cbor::null{}};
    return map({{"id", encode_op_id(op.id)},
                {"obj", encode_obj_id(op.obj)},
                {"key", std::move(key)},
                {"action", cbor::value{static_cast<std::uint64_t>(op.action)}},
                {"value", std::move(*value)},
                {"pred", cbor::value{std::move(predecessors)}},
                {"after", std::move(after)}});
}

auto decode_op(const cbor::value &value) -> result<am::Op> {
    auto id_value = field(value, "id");
    auto obj_value = field(value, "obj");
    auto key_value = field(value, "key");
    auto action = unsigned_field(value, "action");
    auto stored_value = field(value, "value");
    auto pred = array_field(value, "pred");
    auto after_value = field(value, "after");
    if (!id_value || !obj_value || !key_value || !action || !stored_value || !pred ||
        !after_value || *action > static_cast<std::uint64_t>(am::OpType::mark)) {
        return std::unexpected(corrupt("invalid operation", "sync frame"));
    }
    auto id = decode_op_id(**id_value);
    auto obj = decode_obj_id(**obj_value);
    auto decoded = decode_value(**stored_value);
    if (!id || !obj || !decoded)
        return std::unexpected(corrupt("invalid operation", "sync frame"));
    auto key = am::Prop{};
    if (const auto *text = std::get_if<std::string>(&(**key_value).data))
        key = *text;
    else if (const auto *index = std::get_if<std::uint64_t>(&(**key_value).data);
             index && *index <= std::numeric_limits<std::size_t>::max())
        key = static_cast<std::size_t>(*index);
    else
        return std::unexpected(corrupt("invalid operation property", "sync frame"));
    auto predecessors = std::vector<am::OpId>{};
    for (const auto &item : **pred) {
        auto predecessor = decode_op_id(item);
        if (!predecessor)
            return std::unexpected(predecessor.error());
        predecessors.push_back(*predecessor);
    }
    auto after = std::optional<am::OpId>{};
    if (!std::holds_alternative<cbor::null>((**after_value).data)) {
        auto decoded_after = decode_op_id(**after_value);
        if (!decoded_after)
            return std::unexpected(decoded_after.error());
        after = *decoded_after;
    }
    return am::Op{.id = *id,
                  .obj = *obj,
                  .key = std::move(key),
                  .action = static_cast<am::OpType>(*action),
                  .value = std::move(*decoded),
                  .pred = std::move(predecessors),
                  .insert_after = after};
}

auto encode_change(const am::Change &change) -> result<cbor::value> {
    auto operations = cbor::value::array{};
    operations.reserve(change.operations.size());
    for (const auto &op : change.operations) {
        auto encoded = encode_op(op);
        if (!encoded)
            return std::unexpected(encoded.error());
        operations.push_back(std::move(*encoded));
    }
    auto message = change.message ? cbor::value{*change.message} : cbor::value{cbor::null{}};
    return map({{"actor", encode_actor(change.actor)},
                {"seq", cbor::value{change.seq}},
                {"start", cbor::value{change.start_op}},
                {"at", cbor::value{change.timestamp}},
                {"message", std::move(message)},
                {"deps", encode_hashes(change.deps)},
                {"ops", cbor::value{std::move(operations)}}});
}

auto decode_change(const cbor::value &value) -> result<am::Change> {
    auto actor_value = field(value, "actor");
    auto seq = unsigned_field(value, "seq");
    auto start = unsigned_field(value, "start");
    auto at = signed_field(value, "at");
    auto message_value = field(value, "message");
    auto deps_value = field(value, "deps");
    auto operations = array_field(value, "ops");
    if (!actor_value || !seq || !start || !at || !message_value || !deps_value || !operations)
        return std::unexpected(corrupt("invalid change", "sync frame"));
    auto actor = decode_actor(**actor_value);
    auto deps = decode_hashes(**deps_value);
    if (!actor || !deps)
        return std::unexpected(corrupt("invalid change", "sync frame"));
    auto message = std::optional<std::string>{};
    if (const auto *text = std::get_if<std::string>(&(**message_value).data))
        message = *text;
    else if (!std::holds_alternative<cbor::null>((**message_value).data))
        return std::unexpected(corrupt("invalid change message", "sync frame"));
    auto ops = std::vector<am::Op>{};
    ops.reserve((*operations)->size());
    for (const auto &item : **operations) {
        auto op = decode_op(item);
        if (!op)
            return std::unexpected(op.error());
        ops.push_back(std::move(*op));
    }
    return am::Change{.actor = *actor,
                      .seq = *seq,
                      .start_op = *start,
                      .timestamp = *at,
                      .message = std::move(message),
                      .deps = std::move(*deps),
                      .operations = std::move(ops)};
}

auto encode_sync(const am::SyncMessage &message) -> result<std::vector<std::byte>> {
    auto have = cbor::value::array{};
    for (const auto &item : message.have) {
        have.push_back(map(
            {{"last", encode_hashes(item.last_sync)}, {"bloom", cbor::value{item.bloom_bytes}}}));
    }
    auto changes = cbor::value::array{};
    for (const auto &change : message.changes) {
        auto encoded = encode_change(change);
        if (!encoded)
            return std::unexpected(encoded.error());
        changes.push_back(std::move(*encoded));
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"message", map({{"heads", encode_hashes(message.heads)},
                                              {"need", encode_hashes(message.need)},
                                              {"have", cbor::value{std::move(have)}},
                                              {"changes", cbor::value{std::move(changes)}}})}}));
}

auto decode_sync(std::span<const std::byte> bytes) -> result<am::SyncMessage> {
    auto frame = cbor::decode(bytes);
    if (!frame)
        return std::unexpected(frame.error());
    auto version = unsigned_field(*frame, "v");
    auto message_value = field(*frame, "message");
    if (!version || *version != 1 || !message_value)
        return std::unexpected(corrupt("unsupported sync frame", "sync frame"));
    auto heads_value = field(**message_value, "heads");
    auto need_value = field(**message_value, "need");
    auto have_value = array_field(**message_value, "have");
    auto changes_value = array_field(**message_value, "changes");
    if (!heads_value || !need_value || !have_value || !changes_value)
        return std::unexpected(corrupt("invalid sync message", "sync frame"));
    auto heads = decode_hashes(**heads_value);
    auto need = decode_hashes(**need_value);
    if (!heads || !need)
        return std::unexpected(corrupt("invalid sync hashes", "sync frame"));
    auto have = std::vector<am::Have>{};
    for (const auto &item : **have_value) {
        auto last_value = field(item, "last");
        auto bloom_value = field(item, "bloom");
        if (!last_value || !bloom_value)
            return std::unexpected(corrupt("invalid have summary", "sync frame"));
        auto last = decode_hashes(**last_value);
        const auto *bloom = std::get_if<cbor::value::bytes>(&(**bloom_value).data);
        if (!last || !bloom)
            return std::unexpected(corrupt("invalid have summary", "sync frame"));
        have.push_back(am::Have{.last_sync = std::move(*last), .bloom_bytes = *bloom});
    }
    auto changes = std::vector<am::Change>{};
    for (const auto &item : **changes_value) {
        auto change = decode_change(item);
        if (!change)
            return std::unexpected(change.error());
        changes.push_back(std::move(*change));
    }
    return am::SyncMessage{.heads = std::move(*heads),
                           .need = std::move(*need),
                           .have = std::move(have),
                           .changes = std::move(changes)};
}

auto key_for(const head_set &heads) -> std::string {
    auto sorted = heads;
    std::ranges::sort(sorted);
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto result = std::string{};
    result.reserve(sorted.size() * 64);
    for (const auto &current : sorted) {
        for (const auto byte : current) {
            const auto value = std::to_integer<unsigned char>(byte);
            result.push_back(digits[value >> 4U]);
            result.push_back(digits[value & 0x0fU]);
        }
    }
    return result;
}

auto public_heads(const std::vector<am::ChangeHash> &heads) -> head_set {
    auto output = head_set{};
    output.reserve(heads.size());
    std::ranges::transform(heads, std::back_inserter(output),
                           [](const auto &value) { return value.bytes; });
    std::ranges::sort(output);
    return output;
}

auto automerge_heads(const head_set &heads) -> std::vector<am::ChangeHash> {
    auto output = std::vector<am::ChangeHash>{};
    output.reserve(heads.size());
    std::ranges::transform(heads, std::back_inserter(output),
                           [](const auto &value) { return am::ChangeHash{value}; });
    return output;
}

auto make_random_id() -> node_id {
    auto source = std::random_device{};
    auto output = node_id{};
    std::ranges::generate(output, [&] { return std::byte{static_cast<unsigned char>(source())}; });
    return output;
}

auto zero(const node_id &id) -> bool {
    return std::ranges::all_of(id, [](std::byte byte) { return byte == std::byte{0}; });
}

} // namespace

struct sync_state::impl {
    head_set peer_heads;
    head_set last_sent_heads;
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
sync_state::sync_state(sync_state &&) noexcept = default;
auto sync_state::operator=(sync_state &&) noexcept -> sync_state & = default;

document::document(std::unique_ptr<impl> implementation) : impl_(std::move(implementation)) {}
document::document(document &&) noexcept = default;
auto document::operator=(document &&) noexcept -> document & = default;
document::~document() = default;

auto derive_actor_id(std::string_view taap_id, std::string_view device_id) -> result<actor_id> {
    auto input = std::vector<std::byte>{};
    input.reserve(taap_id.size() + device_id.size());
    input.insert(input.end(), reinterpret_cast<const std::byte *>(taap_id.data()),
                 reinterpret_cast<const std::byte *>(taap_id.data() + taap_id.size()));
    input.insert(input.end(), reinterpret_cast<const std::byte *>(device_id.data()),
                 reinterpret_cast<const std::byte *>(device_id.data() + device_id.size()));
    auto digest = liquid::hash(algorithm::blake3, input);
    if (!digest)
        return std::unexpected(digest.error());
    auto output = actor_id{};
    std::ranges::copy_n(digest->bytes.begin(), output.size(), output.begin());
    return output;
}

auto document::create(const repo_id &repository, std::string_view taap_id,
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
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "create CRDT document"));
    }
}

auto document::load(std::span<const std::byte> bytes, std::string_view taap_id,
                    std::string_view device_id) -> result<document> {
    try {
        auto actor = derive_actor_id(taap_id, device_id);
        if (!actor)
            return std::unexpected(actor.error());
        auto loaded = am::Document::load(bytes);
        if (!loaded || !schema::valid_root(*loaded))
            return std::unexpected(corrupt("invalid repository document", "load CRDT document"));
        loaded->set_actor_id(am::ActorId{*actor});
        return document{std::make_unique<impl>(std::move(*loaded), *actor)};
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "load CRDT document"));
    }
}

auto document::actor() const -> actor_id { return impl_->actor; }

auto document::save() const -> result<std::vector<std::byte>> {
    try {
        return impl_->value.save();
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "save CRDT document"));
    }
}

auto document::heads() -> result<head_set> {
    try {
        auto result = public_heads(impl_->value.get_heads());
        auto operations = schema::operations(impl_->value);
        if (!operations)
            return std::unexpected(operations.error());
        auto ids = std::set<node_id>{};
        for (const auto &operation : *operations)
            ids.insert(operation.id);
        impl_->operation_snapshots[key_for(result)] = std::move(ids);
        return result;
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "read CRDT heads"));
    }
}

auto document::transact(std::vector<operation> operations) -> result<std::vector<operation>> {
    try {
        for (auto &operation : operations) {
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
        if (auto applied = schema::apply(impl_->value, operations, impl_->actor); !applied)
            return std::unexpected(applied.error());
        return operations;
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "CRDT transaction"));
    }
}

auto document::diff(const head_set &before_heads) const -> result<std::vector<operation>> {
    auto snapshot = impl_->operation_snapshots.find(key_for(before_heads));
    if (snapshot == impl_->operation_snapshots.end()) {
        return std::unexpected(
            error{errc::invalid_argument, "heads were not observed by this document", "CRDT diff"});
    }
    auto operations = schema::operations(impl_->value);
    if (!operations)
        return std::unexpected(operations.error());
    auto output = std::vector<operation>{};
    std::ranges::copy_if(*operations, std::back_inserter(output),
                         [&](const operation &op) { return !snapshot->second.contains(op.id); });
    return output;
}

auto document::inspect(std::string_view path) const -> result<std::optional<entry>> {
    return schema::inspect(impl_->value, path);
}

auto document::generate_sync_message(sync_state &state) const
    -> result<std::optional<std::vector<std::byte>>> {
    try {
        const auto current = public_heads(impl_->value.get_heads());
        if (current == state.impl_->peer_heads || current == state.impl_->last_sent_heads)
            return std::optional<std::vector<std::byte>>{};
        auto message = am::SyncMessage{.heads = automerge_heads(current),
                                       .changes = impl_->value.get_changes()};
        auto encoded = encode_sync(message);
        if (!encoded)
            return std::unexpected(encoded.error());
        state.impl_->last_sent_heads = current;
        return std::optional<std::vector<std::byte>>{std::move(*encoded)};
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "generate CRDT sync message"));
    }
}

auto document::receive_sync_message(sync_state &state, std::span<const std::byte> bytes)
    -> result<void> {
    try {
        auto decoded = decode_sync(bytes);
        if (!decoded)
            return std::unexpected(decoded.error());
        const auto existing = impl_->value.get_changes();
        auto unseen = std::vector<am::Change>{};
        std::ranges::copy_if(
            decoded->changes, std::back_inserter(unseen), [&](const am::Change &incoming) {
                return std::ranges::none_of(existing, [&](const am::Change &current) {
                    return current.actor == incoming.actor && current.seq == incoming.seq;
                });
            });
        auto candidate = impl_->value;
        candidate.apply_changes(unseen);
        if (!schema::valid_root(candidate))
            return std::unexpected(corrupt("sync produced an invalid document", "receive sync"));
        impl_->value = std::move(candidate);
        state.impl_->peer_heads = public_heads(decoded->heads);
        return {};
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "receive CRDT sync message"));
    }
}

auto document::merge(const document &other) -> result<void> {
    try {
        auto candidate = impl_->value;
        candidate.merge(other.impl_->value);
        if (!schema::valid_root(candidate))
            return std::unexpected(corrupt("merge produced an invalid document", "CRDT merge"));
        impl_->value = std::move(candidate);
        return {};
    } catch (const std::exception &exception) {
        return std::unexpected(internal(exception.what(), "merge CRDT documents"));
    }
}

} // namespace liquid::crdt
