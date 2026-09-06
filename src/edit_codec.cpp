#include "liquid/edit.hpp"

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>

#include "edit_codec.hpp"
#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto codec_error(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "structural edit"};
}

auto map(std::initializer_list<std::pair<std::string, cbor::value>> fields) -> cbor::value {
    auto output = cbor::value::map{};
    output.reserve(fields.size());
    for (const auto& field : fields) {
        output.emplace_back(field.first, field.second);
    }
    return cbor::value{std::move(output)};
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

auto text_field(const cbor::value& value, std::string_view key) -> result<std::string> {
    const auto* child = find(value, key);
    const auto* text = child ? std::get_if<std::string>(&child->data) : nullptr;
    if (!text) {
        return std::unexpected(codec_error("missing or invalid text field: " + std::string{key}));
    }
    return *text;
}

auto unsigned_field(const cbor::value& value, std::string_view key) -> result<std::uint64_t> {
    const auto* child = find(value, key);
    const auto* number = child ? std::get_if<std::uint64_t>(&child->data) : nullptr;
    if (!number) {
        return std::unexpected(
            codec_error("missing or invalid integer field: " + std::string{key}));
    }
    return *number;
}

auto digest_value(const digest& value) -> cbor::value {
    return map({{"alg", static_cast<std::uint64_t>(value.alg)},
                {"bytes", cbor::value::bytes{value.bytes.begin(), value.bytes.end()}}});
}

auto decode_digest(const cbor::value& value) -> result<digest> {
    auto algorithm_value = unsigned_field(value, "alg");
    const auto* bytes_value = find(value, "bytes");
    const auto* bytes = bytes_value ? std::get_if<cbor::value::bytes>(&bytes_value->data) : nullptr;
    if (!algorithm_value || *algorithm_value < 1 || *algorithm_value > 3 || !bytes ||
        bytes->size() != digest{}.bytes.size()) {
        return std::unexpected(codec_error("invalid digest"));
    }
    auto output = digest{static_cast<algorithm>(*algorithm_value), {}};
    std::ranges::copy(*bytes, output.bytes.begin());
    return output;
}

auto position_value(const frontend_source_position& value) -> cbor::value {
    return map({{"byte", value.byte}, {"line", value.line}, {"column", value.column}});
}

auto decode_position(const cbor::value& value) -> result<frontend_source_position> {
    auto byte = unsigned_field(value, "byte");
    auto line = unsigned_field(value, "line");
    auto column = unsigned_field(value, "column");
    if (!byte) {
        return std::unexpected(byte.error());
    }
    if (!line) {
        return std::unexpected(line.error());
    }
    if (!column) {
        return std::unexpected(column.error());
    }
    if (*line == 0 || *column == 0) {
        return std::unexpected(codec_error("edit positions are one-based"));
    }
    return frontend_source_position{*byte, *line, *column};
}

auto range_value(const frontend_source_range& value) -> cbor::value {
    return map({{"start", position_value(value.start)}, {"end", position_value(value.end)}});
}

auto decode_range(const cbor::value& value) -> result<frontend_source_range> {
    const auto* start_value = find(value, "start");
    const auto* end_value = find(value, "end");
    if (!start_value || !end_value) {
        return std::unexpected(codec_error("invalid edit range"));
    }
    auto start = decode_position(*start_value);
    auto end = decode_position(*end_value);
    if (!start) {
        return std::unexpected(start.error());
    }
    if (!end) {
        return std::unexpected(end.error());
    }
    if (end->byte < start->byte) {
        return std::unexpected(codec_error("reversed edit range"));
    }
    return frontend_source_range{*start, *end};
}

auto node_value(const edit_node& value) -> cbor::value {
    return map({{"kind", value.kind},
                {"range", range_value(value.range)},
                {"fingerprint", digest_value(value.fingerprint)}});
}

auto decode_node(const cbor::value& value) -> result<edit_node> {
    auto kind = text_field(value, "kind");
    const auto* range_field = find(value, "range");
    const auto* fingerprint_field = find(value, "fingerprint");
    if (!kind || kind->empty() || !range_field || !fingerprint_field) {
        return std::unexpected(codec_error("invalid edit node"));
    }
    auto range = decode_range(*range_field);
    auto fingerprint = decode_digest(*fingerprint_field);
    if (!range) {
        return std::unexpected(range.error());
    }
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return edit_node{*kind, *range, *fingerprint};
}

auto target_value(const edit_target& value) -> cbor::value {
    return map({{"path", value.path}, {"node", node_value(value.node)}});
}

auto decode_target(const cbor::value& value) -> result<edit_target> {
    auto path = text_field(value, "path");
    const auto* node_field = find(value, "node");
    if (!path || path->empty() || !node_field) {
        return std::unexpected(codec_error("invalid edit target"));
    }
    auto node = decode_node(*node_field);
    if (!node) {
        return std::unexpected(node.error());
    }
    return edit_target{*path, std::move(*node)};
}

auto location_value(const edit_location& value) -> cbor::value {
    if (const auto* symbol = std::get_if<std::string>(&value)) {
        return map({{"symbol", *symbol}});
    }
    const auto& location = std::get<edit_source_location>(value);
    return map({{"path", location.path}, {"line", location.line}, {"column", location.column}});
}

auto decode_location(const cbor::value& value) -> result<edit_location> {
    if (const auto* symbol_value = find(value, "symbol")) {
        const auto* symbol = std::get_if<std::string>(&symbol_value->data);
        if (!symbol || symbol->empty()) {
            return std::unexpected(codec_error("invalid edit symbol location"));
        }
        return edit_location{*symbol};
    }
    auto path = text_field(value, "path");
    auto line = unsigned_field(value, "line");
    auto column = unsigned_field(value, "column");
    if (!path || path->empty() || !line || !column || *line == 0 || *column == 0) {
        return std::unexpected(codec_error("invalid edit source location"));
    }
    return edit_location{edit_source_location{*path, *line, *column}};
}

auto attribution_value(const edit_attribution& value) -> cbor::value {
    auto fields = cbor::value::map{{"tool", value.tool}, {"operation", value.operation}};
    if (value.agent) {
        fields.emplace_back("agent", *value.agent);
    }
    return cbor::value{std::move(fields)};
}

auto decode_attribution(const cbor::value& value) -> result<edit_attribution> {
    auto tool = text_field(value, "tool");
    auto operation = text_field(value, "operation");
    if (!tool || tool->empty() || !operation || operation->empty()) {
        return std::unexpected(codec_error("invalid edit attribution"));
    }
    auto output = edit_attribution{*tool, *operation, std::nullopt};
    if (const auto* agent_value = find(value, "agent")) {
        const auto* agent = std::get_if<std::string>(&agent_value->data);
        if (!agent || agent->empty()) {
            return std::unexpected(codec_error("invalid edit agent attribution"));
        }
        output.agent = *agent;
    }
    return output;
}

auto text_change_value(const text_change& value) -> cbor::value {
    return map({{"range", range_value(value.range)},
                {"old_text", value.old_text},
                {"new_text", value.new_text}});
}

auto decode_text_change(const cbor::value& value) -> result<text_change> {
    const auto* range_field = find(value, "range");
    auto old_text = text_field(value, "old_text");
    auto new_text = text_field(value, "new_text");
    if (!range_field || !old_text || !new_text) {
        return std::unexpected(codec_error("invalid text change"));
    }
    auto range = decode_range(*range_field);
    if (!range) {
        return std::unexpected(range.error());
    }
    return text_change{*range, *old_text, *new_text};
}

auto decode_canonical(std::span<const std::byte> bytes) -> result<cbor::value> {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded || encoded->size() != bytes.size() ||
        !std::equal(encoded->begin(), encoded->end(), bytes.begin())) {
        return std::unexpected(codec_error("edit record is not canonical CBOR"));
    }
    return decoded;
}

auto request_value(const edit_request& value) -> cbor::value {
    return std::visit(
        [](const auto& request) -> cbor::value {
            using request_type = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<request_type, rename_request>) {
                return map({{"v", std::uint64_t{1}},
                            {"kind", "rename"},
                            {"at", location_value(request.at)},
                            {"new_name", request.new_name}});
            } else if constexpr (std::is_same_v<request_type, extract_request>) {
                return map({{"v", std::uint64_t{1}},
                            {"kind", "extract"},
                            {"path", request.path},
                            {"range", range_value(request.range)},
                            {"name", request.name},
                            {"extract_kind",
                             request.kind == extract_kind::function ? cbor::value{"function"}
                                                                    : cbor::value{"value"}}});
            } else if constexpr (std::is_same_v<request_type, move_request>) {
                auto fields = cbor::value::map{{"v", std::uint64_t{1}},
                                               {"kind", "move"},
                                               {"at", location_value(request.at)},
                                               {"to_path", request.to_path}};
                if (request.at_line) {
                    fields.emplace_back("at_line", *request.at_line);
                }
                return cbor::value{std::move(fields)};
            } else {
                auto edits = cbor::value::array{};
                edits.reserve(request.edits.size());
                for (const auto& edit : request.edits) {
                    const auto operation =
                        edit.operation == tree_edit_operation::replace_node    ? "replace_node"
                        : edit.operation == tree_edit_operation::insert_before ? "insert_before"
                        : edit.operation == tree_edit_operation::insert_after  ? "insert_after"
                                                                               : "delete_node";
                    edits.push_back(map({{"target", target_value(edit.target)},
                                         {"operation", operation},
                                         {"text", edit.text}}));
                }
                return map(
                    {{"v", std::uint64_t{1}}, {"kind", "tree_edit"}, {"edits", std::move(edits)}});
            }
        },
        value);
}

auto preview_value(const edit_preview& value, bool include_id) -> cbor::value {
    auto changes = cbor::value::array{};
    changes.reserve(value.changes.size());
    for (const auto& file : value.changes) {
        auto text_changes = cbor::value::array{};
        text_changes.reserve(file.changes.size());
        for (const auto& change : file.changes) {
            text_changes.push_back(text_change_value(change));
        }
        changes.push_back(map({{"path", file.path},
                               {"before", digest_value(file.before)},
                               {"after", digest_value(file.after)},
                               {"changes", std::move(text_changes)}}));
    }
    auto refused = cbor::value::array{};
    refused.reserve(value.refused.size());
    for (const auto& refusal : value.refused) {
        refused.push_back(
            map({{"target", target_value(refusal.target)}, {"reason", refusal.reason}}));
    }
    auto fields = cbor::value::map{{"v", std::uint64_t{1}},
                                   {"kind", "edit_preview"},
                                   {"changes", std::move(changes)},
                                   {"refused", std::move(refused)},
                                   {"attribution", attribution_value(value.attribution)}};
    if (include_id) {
        fields.emplace_back("id", digest_value(value.id));
    }
    return cbor::value{std::move(fields)};
}

auto compute_preview_id(const edit_preview& value) -> result<digest> {
    auto encoded = cbor::encode(preview_value(value, false));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(algorithm::blake3, *encoded);
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

} // namespace

auto edit_detail::preview_id(const edit_preview& value) -> result<digest> {
    return compute_preview_id(value);
}

auto encode_edit_request(const edit_request& value) -> result<std::vector<std::byte>> {
    return cbor::encode(request_value(value));
}

auto decode_edit_request(std::span<const std::byte> value) -> result<edit_request> {
    auto decoded = decode_canonical(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto version = unsigned_field(*decoded, "v");
    auto kind = text_field(*decoded, "kind");
    if (!version || *version != 1 || !kind) {
        return std::unexpected(codec_error("invalid edit request"));
    }
    if (*kind == "rename") {
        const auto* at_value = find(*decoded, "at");
        auto new_name = text_field(*decoded, "new_name");
        if (!at_value || !new_name || new_name->empty()) {
            return std::unexpected(codec_error("invalid rename request"));
        }
        auto at = decode_location(*at_value);
        if (!at) {
            return std::unexpected(at.error());
        }
        return edit_request{rename_request{std::move(*at), *new_name}};
    }
    if (*kind == "extract") {
        auto path = text_field(*decoded, "path");
        auto name = text_field(*decoded, "name");
        auto extract = text_field(*decoded, "extract_kind");
        const auto* range_field = find(*decoded, "range");
        if (!path || path->empty() || !name || name->empty() || !extract || !range_field ||
            (*extract != "function" && *extract != "value")) {
            return std::unexpected(codec_error("invalid extract request"));
        }
        auto range = decode_range(*range_field);
        if (!range) {
            return std::unexpected(range.error());
        }
        return edit_request{
            extract_request{*path,
                            *range,
                            *name,
                            *extract == "function" ? extract_kind::function : extract_kind::value}};
    }
    if (*kind == "move") {
        const auto* at_value = find(*decoded, "at");
        auto to_path = text_field(*decoded, "to_path");
        if (!at_value || !to_path || to_path->empty()) {
            return std::unexpected(codec_error("invalid move request"));
        }
        auto at = decode_location(*at_value);
        if (!at) {
            return std::unexpected(at.error());
        }
        auto at_line = std::optional<std::uint64_t>{};
        if (find(*decoded, "at_line")) {
            auto line = unsigned_field(*decoded, "at_line");
            if (!line || *line == 0) {
                return std::unexpected(codec_error("invalid move insertion line"));
            }
            at_line = *line;
        }
        return edit_request{move_request{std::move(*at), *to_path, at_line}};
    }
    if (*kind != "tree_edit") {
        return std::unexpected(codec_error("unknown edit request kind"));
    }
    const auto* edits_value = find(*decoded, "edits");
    const auto* edits = edits_value ? std::get_if<cbor::value::array>(&edits_value->data) : nullptr;
    if (!edits || edits->empty()) {
        return std::unexpected(codec_error("tree edit request has no edits"));
    }
    auto output = tree_edit_request{};
    output.edits.reserve(edits->size());
    for (const auto& item : *edits) {
        const auto* target_field = find(item, "target");
        auto operation = text_field(item, "operation");
        auto text = text_field(item, "text");
        if (!target_field || !operation || !text) {
            return std::unexpected(codec_error("invalid tree edit"));
        }
        auto target = decode_target(*target_field);
        if (!target) {
            return std::unexpected(target.error());
        }
        auto operation_value = tree_edit_operation::replace_node;
        if (*operation == "insert_before") {
            operation_value = tree_edit_operation::insert_before;
        } else if (*operation == "insert_after") {
            operation_value = tree_edit_operation::insert_after;
        } else if (*operation == "delete_node") {
            operation_value = tree_edit_operation::delete_node;
        } else if (*operation != "replace_node") {
            return std::unexpected(codec_error("unknown tree edit operation"));
        }
        output.edits.push_back({std::move(*target), operation_value, *text});
    }
    return edit_request{std::move(output)};
}

auto encode_edit_preview(const edit_preview& value) -> result<std::vector<std::byte>> {
    auto expected = compute_preview_id(value);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    if (!same_digest(*expected, value.id)) {
        return std::unexpected(codec_error("edit preview id does not match its canonical body"));
    }
    return cbor::encode(preview_value(value, true));
}

auto decode_edit_preview(std::span<const std::byte> value) -> result<edit_preview> {
    auto decoded = decode_canonical(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto version = unsigned_field(*decoded, "v");
    auto kind = text_field(*decoded, "kind");
    const auto* changes_value = find(*decoded, "changes");
    const auto* refused_value = find(*decoded, "refused");
    const auto* attribution_field = find(*decoded, "attribution");
    const auto* id_field = find(*decoded, "id");
    const auto* changes =
        changes_value ? std::get_if<cbor::value::array>(&changes_value->data) : nullptr;
    const auto* refused =
        refused_value ? std::get_if<cbor::value::array>(&refused_value->data) : nullptr;
    if (!version || *version != 1 || !kind || *kind != "edit_preview" || !changes || !refused ||
        !attribution_field || !id_field) {
        return std::unexpected(codec_error("invalid edit preview"));
    }
    auto attribution = decode_attribution(*attribution_field);
    auto id = decode_digest(*id_field);
    if (!attribution) {
        return std::unexpected(attribution.error());
    }
    if (!id) {
        return std::unexpected(id.error());
    }
    auto output = edit_preview{.attribution = std::move(*attribution), .id = *id};
    output.changes.reserve(changes->size());
    for (const auto& item : *changes) {
        auto path = text_field(item, "path");
        const auto* before_field = find(item, "before");
        const auto* after_field = find(item, "after");
        const auto* text_changes_value = find(item, "changes");
        const auto* text_changes = text_changes_value
                                       ? std::get_if<cbor::value::array>(&text_changes_value->data)
                                       : nullptr;
        if (!path || path->empty() || !before_field || !after_field || !text_changes ||
            text_changes->empty()) {
            return std::unexpected(codec_error("invalid preview file edit"));
        }
        auto before = decode_digest(*before_field);
        auto after = decode_digest(*after_field);
        if (!before || !after) {
            return std::unexpected(before ? after.error() : before.error());
        }
        auto file = file_edit{*path, *before, *after, {}};
        file.changes.reserve(text_changes->size());
        for (const auto& child : *text_changes) {
            auto change = decode_text_change(child);
            if (!change) {
                return std::unexpected(change.error());
            }
            file.changes.push_back(std::move(*change));
        }
        output.changes.push_back(std::move(file));
    }
    output.refused.reserve(refused->size());
    for (const auto& item : *refused) {
        const auto* target_field = find(item, "target");
        auto reason = text_field(item, "reason");
        if (!target_field || !reason || reason->empty()) {
            return std::unexpected(codec_error("invalid edit refusal"));
        }
        auto target = decode_target(*target_field);
        if (!target) {
            return std::unexpected(target.error());
        }
        output.refused.push_back({std::move(*target), *reason});
    }
    auto expected = compute_preview_id(output);
    if (!expected || !same_digest(*expected, output.id)) {
        return std::unexpected(expected ? codec_error("edit preview id mismatch")
                                        : expected.error());
    }
    return output;
}

auto encode_edit_result(const edit_result& value) -> result<std::vector<std::byte>> {
    auto drips = cbor::value::array{};
    drips.reserve(value.drips.size());
    for (const auto& drip : value.drips) {
        drips.push_back(digest_value(drip.value));
    }
    return cbor::encode(map({{"v", std::uint64_t{1}},
                             {"kind", "edit_result"},
                             {"drips", std::move(drips)},
                             {"operation", digest_value(value.operation.value)}}));
}

auto decode_edit_result(std::span<const std::byte> value) -> result<edit_result> {
    auto decoded = decode_canonical(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto version = unsigned_field(*decoded, "v");
    auto kind = text_field(*decoded, "kind");
    const auto* drips_value = find(*decoded, "drips");
    const auto* operation_value = find(*decoded, "operation");
    const auto* drips = drips_value ? std::get_if<cbor::value::array>(&drips_value->data) : nullptr;
    if (!version || *version != 1 || !kind || *kind != "edit_result" || !drips ||
        !operation_value) {
        return std::unexpected(codec_error("invalid edit result"));
    }
    auto operation = decode_digest(*operation_value);
    if (!operation) {
        return std::unexpected(operation.error());
    }
    auto output = edit_result{.operation = {*operation}};
    output.drips.reserve(drips->size());
    for (const auto& item : *drips) {
        auto id = decode_digest(item);
        if (!id) {
            return std::unexpected(id.error());
        }
        output.drips.push_back({*id});
    }
    return output;
}

} // namespace liquid
