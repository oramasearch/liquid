// SPDX-License-Identifier: MIT

#include "sync/codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include <zstd.h>

#include "encoding/cbor.hpp"
#include "objects/records.hpp"

namespace liquid::sync_detail {
namespace {

namespace cbor = encoding::cbor;

constexpr auto maximum_frame_size = std::size_t{16U * 1024U * 1024U};
constexpr auto physical_header_size = std::size_t{49};
constexpr auto frame_magic = std::array<std::byte, 8>{std::byte{'L'},
                                                      std::byte{'Q'},
                                                      std::byte{'F'},
                                                      std::byte{'R'},
                                                      std::byte{'A'},
                                                      std::byte{'M'},
                                                      std::byte{1},
                                                      std::byte{0}};

auto failure(std::string message, std::string context = "sync protocol") -> error {
    return {errc::corrupt, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
    return cbor::value{cbor::value::map{values}};
}

auto bytes(std::span<const std::byte> value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}

auto actor_value(const actor& value) -> cbor::value {
    return map({{"taap_id", value.taap_id},
                {"algorithm", value.algorithm},
                {"fingerprint", value.fingerprint},
                {"public_key", bytes(value.public_key)},
                {"key_fingerprint", value.key_fingerprint}});
}

auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [name, child] : *fields) {
        const auto* text = std::get_if<std::string>(&name.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
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
        return std::unexpected(failure("invalid actor"));
    }
    return actor{*taap_id_text,
                 *algorithm_text,
                 *fingerprint_text,
                 *public_key_bytes,
                 *key_fingerprint_text};
}

auto digest_array(std::span<const digest> values) -> cbor::value {
    auto output = cbor::value::array{};
    output.reserve(values.size());
    for (const auto& value : values) {
        output.emplace_back(to_string(value));
    }
    return cbor::value{std::move(output)};
}

auto decode_digests(const cbor::value& value) -> result<std::vector<digest>> {
    const auto* input = std::get_if<cbor::value::array>(&value.data);
    if (!input) {
        return std::unexpected(failure("object list is not an array"));
    }
    auto output = std::vector<digest>{};
    output.reserve(input->size());
    for (const auto& item : *input) {
        const auto* text = std::get_if<std::string>(&item.data);
        if (!text) {
            return std::unexpected(failure("object identity is not text"));
        }
        auto parsed = parse_digest(*text);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.push_back(*parsed);
    }
    return output;
}

auto outcome_array(std::span<const sync_ref_outcome> values) -> cbor::value {
    auto output = cbor::value::array{};
    output.reserve(values.size());
    for (const auto& value : values) {
        auto fields = cbor::value::map{
            {"name", value.name},
            {"outcome",
             value.outcome == sync_ref_outcome_kind::applied ? cbor::value{"applied"}
                                                             : cbor::value{"refused"}},
        };
        if (value.outcome == sync_ref_outcome_kind::refused) {
            fields.emplace_back("reason", value.reason);
        }
        output.emplace_back(cbor::value{std::move(fields)});
    }
    return cbor::value{std::move(output)};
}

auto decode_outcomes(const cbor::value& value) -> result<std::vector<sync_ref_outcome>> {
    const auto* input = std::get_if<cbor::value::array>(&value.data);
    if (!input) {
        return std::unexpected(failure("reference outcomes are not an array"));
    }
    auto output = std::vector<sync_ref_outcome>{};
    output.reserve(input->size());
    for (const auto& item : *input) {
        const auto* name_field = find(item, "name");
        const auto* outcome_field = find(item, "outcome");
        const auto* reason_field = find(item, "reason");
        const auto* name = name_field ? std::get_if<std::string>(&name_field->data) : nullptr;
        const auto* outcome =
            outcome_field ? std::get_if<std::string>(&outcome_field->data) : nullptr;
        const auto* reason = reason_field ? std::get_if<std::string>(&reason_field->data) : nullptr;
        if (!name || !outcome || (*outcome != "applied" && *outcome != "refused") ||
            (*outcome == "refused" && !reason)) {
            return std::unexpected(failure("invalid reference outcome"));
        }
        output.push_back(sync_ref_outcome{
            .name = *name,
            .outcome = *outcome == "applied" ? sync_ref_outcome_kind::applied
                                             : sync_ref_outcome_kind::refused,
            .reason = reason ? *reason : std::string{},
        });
    }
    return output;
}

void put_u32_be(std::vector<std::byte>& output, std::uint32_t value) {
    for (const auto shift : {24U, 16U, 8U, 0U}) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

void put_u32_le(std::vector<std::byte>& output, std::uint32_t value) {
    for (const auto shift : {0U, 8U, 16U, 24U}) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

auto get_u32_be(std::span<const std::byte> input, std::size_t offset) -> std::uint32_t {
    auto output = std::uint32_t{};
    for (auto index = std::size_t{}; index < 4; ++index) {
        output = (output << 8U) | std::to_integer<std::uint8_t>(input[offset + index]);
    }
    return output;
}

auto get_u32_le(std::span<const std::byte> input, std::size_t offset) -> std::uint32_t {
    auto output = std::uint32_t{};
    for (auto index = std::size_t{}; index < 4; ++index) {
        output |= std::uint32_t{std::to_integer<std::uint8_t>(input[offset + index])}
                  << (index * 8U);
    }
    return output;
}

auto append_frame(std::vector<std::byte>& output, std::uint8_t type, const cbor::value& payload)
    -> result<void> {
    auto encoded = cbor::encode(payload);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (encoded->size() > maximum_frame_size) {
        return std::unexpected(failure("metadata frame exceeds the size limit"));
    }
    output.push_back(std::byte{type});
    put_u32_be(output, static_cast<std::uint32_t>(encoded->size()));
    output.insert(output.end(), encoded->begin(), encoded->end());
    return {};
}

auto append_end(std::vector<std::byte>& output) -> void {
    output.push_back(std::byte{255});
    put_u32_be(output, 0);
}

auto decode_item(std::uint8_t type, const cbor::value& payload) -> result<sync_item> {
    if (type == static_cast<std::uint8_t>(sync_item_kind::drip)) {
        const auto* author_field = find(payload, "author");
        const auto* body_field = find(payload, "body");
        const auto* signature_field = find(payload, "signature");
        auto author = author_field ? decode_actor(*author_field)
                                   : result<actor>{std::unexpected(failure("drip author missing"))};
        const auto* body =
            body_field ? std::get_if<cbor::value::bytes>(&body_field->data) : nullptr;
        const auto* signature =
            signature_field ? std::get_if<cbor::value::bytes>(&signature_field->data) : nullptr;
        if (!author || !body || !signature) {
            return std::unexpected(author ? failure("invalid drip frame") : author.error());
        }
        return sync_item{.kind = sync_item_kind::drip,
                         .author = std::move(*author),
                         .body = *body,
                         .signature = *signature};
    }
    if (type == static_cast<std::uint8_t>(sync_item_kind::change)) {
        const auto* author_field = find(payload, "author");
        const auto* body_field = find(payload, "body");
        const auto* id_field = find(payload, "id");
        auto author = author_field
                          ? decode_actor(*author_field)
                          : result<actor>{std::unexpected(failure("change author missing"))};
        const auto* body =
            body_field ? std::get_if<cbor::value::bytes>(&body_field->data) : nullptr;
        const auto* id = id_field ? std::get_if<std::string>(&id_field->data) : nullptr;
        if (!author || !body || !id) {
            return std::unexpected(failure("invalid change frame"));
        }
        auto parsed = parse_digest(*id);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto value = objects::decode_change(*body);
        if (!value) {
            return std::unexpected(value.error());
        }
        return sync_item{.kind = sync_item_kind::change,
                         .author = std::move(*author),
                         .reference = change_id{*parsed},
                         .body = *body,
                         .signature = value->sig};
    }
    if (type == static_cast<std::uint8_t>(sync_item_kind::directory)) {
        const auto* repository_field = find(payload, "repository");
        const auto* node_field = find(payload, "node");
        const auto* message_field = find(payload, "message");
        const auto* root_field = find(payload, "root");
        const auto* repository =
            repository_field ? std::get_if<cbor::value::bytes>(&repository_field->data) : nullptr;
        const auto* node =
            node_field ? std::get_if<cbor::value::bytes>(&node_field->data) : nullptr;
        const auto* message =
            message_field ? std::get_if<cbor::value::bytes>(&message_field->data) : nullptr;
        const auto* root = root_field ? std::get_if<bool>(&root_field->data) : nullptr;
        if (!repository || repository->size() != node_id{}.size() || !node ||
            node->size() != node_id{}.size() || !message || !root) {
            return std::unexpected(failure("invalid directory frame"));
        }
        auto output = sync_item{.kind = sync_item_kind::directory, .root = *root, .body = *message};
        std::ranges::copy(*repository, output.repository.begin());
        std::ranges::copy(*node, output.directory.begin());
        return output;
    }
    if (type == static_cast<std::uint8_t>(sync_item_kind::reference)) {
        const auto* name_field = find(payload, "name");
        const auto* change_field = find(payload, "change");
        const auto* name = name_field ? std::get_if<std::string>(&name_field->data) : nullptr;
        const auto* change = change_field ? std::get_if<std::string>(&change_field->data) : nullptr;
        if (!name || !change) {
            return std::unexpected(failure("invalid reference frame"));
        }
        auto parsed = parse_digest(*change);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return sync_item{
            .kind = sync_item_kind::reference, .name = *name, .reference = change_id{*parsed}};
    }
    if (type == static_cast<std::uint8_t>(sync_item_kind::presence)) {
        const auto* body_field = find(payload, "body");
        const auto* body =
            body_field ? std::get_if<cbor::value::bytes>(&body_field->data) : nullptr;
        if (!body) {
            return std::unexpected(failure("invalid presence frame"));
        }
        return sync_item{.kind = sync_item_kind::presence, .body = *body};
    }
    if (type == static_cast<std::uint8_t>(sync_item_kind::identity)) {
        const auto* author_field = find(payload, "author");
        const auto* body_field = find(payload, "body");
        auto author = author_field
                          ? decode_actor(*author_field)
                          : result<actor>{std::unexpected(failure("identity author missing"))};
        const auto* body =
            body_field ? std::get_if<cbor::value::bytes>(&body_field->data) : nullptr;
        if (!author || !body) {
            return std::unexpected(author ? failure("invalid identity frame") : author.error());
        }
        return sync_item{
            .kind = sync_item_kind::identity, .author = std::move(*author), .body = *body};
    }
    return std::unexpected(failure("unknown metadata frame type"));
}

auto item_value(const sync_item& item) -> cbor::value {
    if (item.kind == sync_item_kind::drip) {
        return map({{"author", actor_value(item.author)},
                    {"body", bytes(item.body)},
                    {"signature", bytes(item.signature)}});
    }
    if (item.kind == sync_item_kind::change) {
        return map({{"author", actor_value(item.author)},
                    {"id", to_string(item.reference.value)},
                    {"body", bytes(item.body)}});
    }
    if (item.kind == sync_item_kind::directory) {
        return map({{"repository", bytes(item.repository)},
                    {"node", bytes(item.directory)},
                    {"root", item.root},
                    {"message", bytes(item.body)}});
    }
    if (item.kind == sync_item_kind::presence) {
        return map({{"body", bytes(item.body)}});
    }
    if (item.kind == sync_item_kind::identity) {
        return map({{"author", actor_value(item.author)}, {"body", bytes(item.body)}});
    }
    return map({{"name", item.name}, {"change", to_string(item.reference.value)}});
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

} // namespace

auto encode_metadata(const metadata_document& document) -> result<std::vector<std::byte>> {
    auto objects = document.objects;
    std::ranges::sort(objects, {}, [](const digest& id) { return to_string(id); });
    objects.erase(std::unique(objects.begin(), objects.end(), same_digest), objects.end());
    auto refs = cbor::value::array{};
    for (const auto& item : document.items) {
        if (item.kind == sync_item_kind::reference) {
            refs.push_back(map({{"name", item.name}, {"change", to_string(item.reference.value)}}));
        }
    }
    std::ranges::sort(refs, {}, [](const cbor::value& value) {
        const auto* field = find(value, "name");
        const auto* text = field ? std::get_if<std::string>(&field->data) : nullptr;
        return text ? *text : std::string{};
    });
    auto output = std::vector<std::byte>{};
    auto header_fields = cbor::value::map{{"v", std::uint64_t{1}},
                                          {"cursor", document.cursor.token},
                                          {"refs", cbor::value{std::move(refs)}},
                                          {"objects", digest_array(objects)}};
    if (document.atomic) {
        header_fields.emplace_back("atomic", true);
    }
    if (!document.outcomes.empty()) {
        header_fields.emplace_back("outcomes", outcome_array(document.outcomes));
    }
    auto header = cbor::value{std::move(header_fields)};
    if (auto appended = append_frame(output, 1, header); !appended) {
        return std::unexpected(appended.error());
    }
    for (const auto& item : document.items) {
        if (auto appended =
                append_frame(output, static_cast<std::uint8_t>(item.kind) + 1U, item_value(item));
            !appended) {
            return std::unexpected(appended.error());
        }
    }
    append_end(output);
    return output;
}

auto decode_metadata(std::span<const std::byte> input) -> result<metadata_document> {
    auto output = metadata_document{};
    auto offset = std::size_t{};
    auto saw_header = false;
    auto saw_end = false;
    while (offset < input.size()) {
        if (input.size() - offset < 5) {
            return std::unexpected(failure("truncated metadata frame header"));
        }
        const auto type = std::to_integer<std::uint8_t>(input[offset]);
        const auto length = get_u32_be(input, offset + 1);
        offset += 5;
        if (length > maximum_frame_size || length > input.size() - offset) {
            return std::unexpected(failure("invalid metadata frame length"));
        }
        const auto payload = input.subspan(offset, length);
        offset += length;
        if (type == 255) {
            if (length != 0 || offset != input.size()) {
                return std::unexpected(failure("invalid metadata end frame"));
            }
            saw_end = true;
            break;
        }
        auto decoded = cbor::decode(payload);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!saw_header) {
            if (type != 1) {
                return std::unexpected(failure("metadata header is missing"));
            }
            const auto* version_field = find(*decoded, "v");
            const auto* cursor_field = find(*decoded, "cursor");
            const auto* objects_field = find(*decoded, "objects");
            const auto* atomic_field = find(*decoded, "atomic");
            const auto* outcomes_field = find(*decoded, "outcomes");
            const auto* version =
                version_field ? std::get_if<std::uint64_t>(&version_field->data) : nullptr;
            const auto* cursor =
                cursor_field ? std::get_if<std::string>(&cursor_field->data) : nullptr;
            const auto* atomic = atomic_field ? std::get_if<bool>(&atomic_field->data) : nullptr;
            if (!version || *version != 1 || !cursor || !objects_field) {
                return std::unexpected(failure("invalid metadata header"));
            }
            if (atomic_field && !atomic) {
                return std::unexpected(failure("invalid metadata atomic flag"));
            }
            auto objects = decode_digests(*objects_field);
            if (!objects) {
                return std::unexpected(objects.error());
            }
            auto outcomes = std::vector<sync_ref_outcome>{};
            if (outcomes_field) {
                auto decoded_outcomes = decode_outcomes(*outcomes_field);
                if (!decoded_outcomes) {
                    return std::unexpected(decoded_outcomes.error());
                }
                outcomes = std::move(*decoded_outcomes);
            }
            output.cursor.token = *cursor;
            output.objects = std::move(*objects);
            output.outcomes = std::move(outcomes);
            output.atomic = atomic ? *atomic : false;
            saw_header = true;
            continue;
        }
        auto item = decode_item(type - 1U, *decoded);
        if (!item) {
            return std::unexpected(item.error());
        }
        output.items.push_back(std::move(*item));
    }
    if (!saw_header || !saw_end) {
        return std::unexpected(failure("incomplete metadata stream"));
    }
    return output;
}

auto encode_object_list(std::span<const digest> objects) -> result<std::vector<std::byte>> {
    return encode_metadata(metadata_document{.objects = {objects.begin(), objects.end()}});
}

auto decode_object_list(std::span<const std::byte> input) -> result<std::vector<digest>> {
    auto decoded = decode_metadata(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (!decoded->items.empty()) {
        return std::unexpected(failure("object list contains metadata items"));
    }
    return std::move(decoded->objects);
}

auto encode_pack_frame(digest id, std::span<const std::byte> contents)
    -> result<std::vector<std::byte>> {
    if (contents.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(failure("object exceeds the pack-frame size limit"));
    }
    const auto bound = ZSTD_compressBound(contents.size());
    auto compressed = std::vector<std::byte>(bound);
    const auto size =
        ZSTD_compress(compressed.data(), compressed.size(), contents.data(), contents.size(), 3);
    if (ZSTD_isError(size) || size > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(failure("object compression failed", ZSTD_getErrorName(size)));
    }
    compressed.resize(size);
    auto output = std::vector<std::byte>{frame_magic.begin(), frame_magic.end()};
    put_u32_le(output, static_cast<std::uint32_t>(contents.size()));
    put_u32_le(output, static_cast<std::uint32_t>(compressed.size()));
    output.push_back(std::byte{static_cast<std::uint8_t>(id.alg)});
    output.insert(output.end(), id.bytes.begin(), id.bytes.end());
    output.insert(output.end(), compressed.begin(), compressed.end());
    return output;
}

auto decode_pack_frames(std::span<const std::byte> input)
    -> result<std::vector<std::pair<digest, std::vector<std::byte>>>> {
    auto output = std::vector<std::pair<digest, std::vector<std::byte>>>{};
    auto offset = std::size_t{};
    while (offset < input.size()) {
        if (input.size() - offset < physical_header_size ||
            !std::equal(frame_magic.begin(), frame_magic.end(), input.begin() + offset)) {
            return std::unexpected(failure("invalid pack frame"));
        }
        const auto raw_size = get_u32_le(input, offset + 8);
        const auto compressed_size = get_u32_le(input, offset + 12);
        const auto algorithm_value = std::to_integer<std::uint8_t>(input[offset + 16]);
        if (raw_size > maximum_frame_size || compressed_size > maximum_frame_size ||
            compressed_size > input.size() - offset - physical_header_size ||
            algorithm_value < static_cast<std::uint8_t>(algorithm::blake3) ||
            algorithm_value > static_cast<std::uint8_t>(algorithm::blake3_keyed)) {
            return std::unexpected(failure("invalid pack frame lengths or algorithm"));
        }
        auto id = digest{static_cast<algorithm>(algorithm_value), {}};
        std::ranges::copy_n(input.begin() + offset + 17, id.bytes.size(), id.bytes.begin());
        auto raw = std::vector<std::byte>(raw_size);
        const auto produced = ZSTD_decompress(
            raw.data(), raw.size(), input.data() + offset + physical_header_size, compressed_size);
        if (ZSTD_isError(produced) || produced != raw.size()) {
            return std::unexpected(failure("pack frame decompression failed"));
        }
        auto actual = hash(id.alg, raw);
        if (!actual || !same_digest(*actual, id)) {
            return std::unexpected(failure("pack frame digest verification failed"));
        }
        output.emplace_back(id, std::move(raw));
        offset += physical_header_size + compressed_size;
    }
    return output;
}

} // namespace liquid::sync_detail
