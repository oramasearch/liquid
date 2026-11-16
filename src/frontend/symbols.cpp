// SPDX-License-Identifier: MIT

#include "liquid/frontend.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto symbol_error(errc code, std::string message) -> error {
    return error{code, std::move(message), "symbols"};
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

auto map(std::initializer_list<std::pair<std::string, cbor::value>> fields) -> cbor::value {
    auto output = cbor::value::map{};
    output.reserve(fields.size());
    for (const auto& field : fields) {
        output.emplace_back(field.first, field.second);
    }
    return cbor::value{std::move(output)};
}

auto encode_digest(const digest& value) -> cbor::value {
    return map({{"alg", static_cast<std::uint64_t>(value.alg)},
                {"bytes", cbor::value::bytes{value.bytes.begin(), value.bytes.end()}}});
}

auto decode_digest(const cbor::value& value) -> result<digest> {
    const auto* alg = find(value, "alg");
    const auto* bytes = find(value, "bytes");
    const auto* number = alg ? std::get_if<std::uint64_t>(&alg->data) : nullptr;
    const auto* raw = bytes ? std::get_if<cbor::value::bytes>(&bytes->data) : nullptr;
    if (!number || !raw || raw->size() != 32 || *number < 1 || *number > 3) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid symbol subject"));
    }
    auto output = digest{static_cast<algorithm>(*number), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto encode_position(const frontend_source_position& value) -> cbor::value {
    return map({{"byte", value.byte}, {"line", value.line}, {"column", value.column}});
}

auto decode_position(const cbor::value& value) -> result<frontend_source_position> {
    const auto* byte = find(value, "byte");
    const auto* line = find(value, "line");
    const auto* column = find(value, "column");
    const auto* offset = byte ? std::get_if<std::uint64_t>(&byte->data) : nullptr;
    const auto* row = line ? std::get_if<std::uint64_t>(&line->data) : nullptr;
    const auto* cell = column ? std::get_if<std::uint64_t>(&column->data) : nullptr;
    if (!offset || !row || !cell) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid symbol position"));
    }
    return frontend_source_position{*offset, *row, *cell};
}

auto encode_range(const frontend_source_range& value) -> cbor::value {
    return map({{"start", encode_position(value.start)}, {"end", encode_position(value.end)}});
}

auto decode_range(const cbor::value& value) -> result<frontend_source_range> {
    const auto* start = find(value, "start");
    const auto* end = find(value, "end");
    if (!start || !end) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid symbol range"));
    }
    auto first = decode_position(*start);
    auto last = decode_position(*end);
    if (!first || !last || last->byte < first->byte) {
        return std::unexpected(symbol_error(errc::corrupt, "reversed symbol range"));
    }
    return frontend_source_range{*first, *last};
}

auto kind_from_text(std::string_view value) -> symbol_kind {
    if (value == "function") {
        return symbol_kind::function;
    }
    if (value == "type") {
        return symbol_kind::type;
    }
    if (value == "variable") {
        return symbol_kind::variable;
    }
    if (value == "field") {
        return symbol_kind::field;
    }
    if (value == "parameter") {
        return symbol_kind::parameter;
    }
    if (value == "namespace") {
        return symbol_kind::namespace_;
    }
    return symbol_kind::unknown;
}

auto kind_text(symbol_kind value) -> std::string_view {
    switch (value) {
    case symbol_kind::function:
        return "function";
    case symbol_kind::type:
        return "type";
    case symbol_kind::variable:
        return "variable";
    case symbol_kind::field:
        return "field";
    case symbol_kind::parameter:
        return "parameter";
    case symbol_kind::namespace_:
        return "namespace";
    default:
        return "unknown";
    }
}

auto decode_occurrences(const cbor::value& value) -> result<std::vector<symbol_occurrence>> {
    const auto* values = std::get_if<cbor::value::array>(&value.data);
    if (!values) {
        return std::unexpected(symbol_error(errc::corrupt, "symbol occurrences are not an array"));
    }
    auto output = std::vector<symbol_occurrence>{};
    output.reserve(values->size());
    for (const auto& item : *values) {
        const auto* symbol = find(item, "symbol");
        const auto* name = find(item, "name");
        const auto* kind = find(item, "kind");
        const auto* role = find(item, "role");
        const auto* range = find(item, "range");
        const auto* id = symbol ? std::get_if<std::string>(&symbol->data) : nullptr;
        const auto* text = name ? std::get_if<std::string>(&name->data) : nullptr;
        const auto* category = kind ? std::get_if<std::string>(&kind->data) : nullptr;
        const auto* use = role ? std::get_if<std::string>(&role->data) : nullptr;
        if (!id || id->empty() || !text || text->empty() || !category || !use || !range) {
            return std::unexpected(symbol_error(errc::corrupt, "invalid symbol occurrence"));
        }
        auto decoded_range = decode_range(*range);
        if (!decoded_range || (*use != "definition" && *use != "reference")) {
            return std::unexpected(symbol_error(errc::corrupt, "invalid symbol occurrence"));
        }
        auto occurrence = symbol_occurrence{*id,
                                            *text,
                                            kind_from_text(*category),
                                            *use == "definition" ? symbol_role::definition
                                                                 : symbol_role::reference,
                                            *decoded_range,
                                            std::nullopt,
                                            std::nullopt};
        if (const auto* container = find(item, "container")) {
            if (const auto* text_value = std::get_if<std::string>(&container->data)) {
                occurrence.container = *text_value;
            } else {
                return std::unexpected(symbol_error(errc::corrupt, "invalid symbol container"));
            }
        }
        if (const auto* documentation = find(item, "documentation")) {
            if (const auto* text_value = std::get_if<std::string>(&documentation->data)) {
                occurrence.documentation = *text_value;
            } else {
                return std::unexpected(symbol_error(errc::corrupt, "invalid symbol documentation"));
            }
        }
        output.push_back(std::move(occurrence));
    }
    return output;
}

auto decode_value(std::span<const std::byte> bytes) -> result<cbor::value> {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded || encoded->size() != bytes.size() ||
        !std::equal(encoded->begin(), encoded->end(), bytes.begin())) {
        return std::unexpected(symbol_error(errc::corrupt, "symbol record is not canonical CBOR"));
    }
    return decoded;
}

auto decode_with_subject(std::optional<digest> subject, std::span<const std::byte> bytes)
    -> result<symbol_record> {
    auto decoded = decode_value(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version = find(*decoded, "v");
    const auto* record_kind = find(*decoded, "kind");
    const auto* language = find(*decoded, "language");
    const auto* origin = find(*decoded, "origin");
    const auto* grammar = find(*decoded, "grammar");
    const auto* occurrences = find(*decoded, "occurrences");
    const auto* number = version ? std::get_if<std::uint64_t>(&version->data) : nullptr;
    const auto* kind_text = record_kind ? std::get_if<std::string>(&record_kind->data) : nullptr;
    const auto* language_text = language ? std::get_if<std::string>(&language->data) : nullptr;
    const auto* origin_text = origin ? std::get_if<std::string>(&origin->data) : nullptr;
    const auto* grammar_text = grammar ? std::get_if<std::string>(&grammar->data) : nullptr;
    if (!number || *number != 1 || (kind_text && *kind_text != "symbols") || !language_text ||
        !origin_text || !occurrences ||
        (*origin_text != "compiler" && *origin_text != "syntactic")) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid symbol record"));
    }
    if (!subject && (!kind_text || *kind_text != "symbols")) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid stored symbol record"));
    }
    if (!subject) {
        const auto* encoded_subject = find(*decoded, "subject");
        if (!encoded_subject) {
            return std::unexpected(symbol_error(errc::corrupt, "symbol record has no subject"));
        }
        auto decoded_subject = decode_digest(*encoded_subject);
        if (!decoded_subject) {
            return std::unexpected(decoded_subject.error());
        }
        subject = *decoded_subject;
    }
    auto decoded_occurrences = decode_occurrences(*occurrences);
    if (!decoded_occurrences) {
        return std::unexpected(decoded_occurrences.error());
    }
    if (grammar && !grammar_text) {
        return std::unexpected(symbol_error(errc::corrupt, "invalid symbol grammar digest"));
    }
    return symbol_record{
        .subject = *subject,
        .language = *language_text,
        .origin = *origin_text == "compiler" ? symbol_origin::compiler : symbol_origin::syntactic,
        .grammar = grammar_text ? *grammar_text : std::string{},
        .occurrences = std::move(*decoded_occurrences),
    };
}

} // namespace

auto encode_symbols(const symbol_record& value) -> result<std::vector<std::byte>> {
    auto occurrences = cbor::value::array{};
    occurrences.reserve(value.occurrences.size());
    for (const auto& occurrence : value.occurrences) {
        auto fields = cbor::value::map{{"symbol", occurrence.symbol},
                                       {"name", occurrence.name},
                                       {"kind", std::string{kind_text(occurrence.kind)}},
                                       {"role",
                                        occurrence.role == symbol_role::definition
                                            ? cbor::value{"definition"}
                                            : cbor::value{"reference"}},
                                       {"range", encode_range(occurrence.range)}};
        if (occurrence.container) {
            fields.emplace_back("container", *occurrence.container);
        }
        if (occurrence.documentation) {
            fields.emplace_back("documentation", *occurrence.documentation);
        }
        occurrences.emplace_back(std::move(fields));
    }
    auto fields =
        cbor::value::map{{"v", std::uint64_t{1}},
                         {"kind", "symbols"},
                         {"subject", encode_digest(value.subject)},
                         {"language", value.language},
                         {"origin",
                          value.origin == symbol_origin::compiler ? cbor::value{"compiler"}
                                                                  : cbor::value{"syntactic"}},
                         {"occurrences", std::move(occurrences)}};
    if (!value.grammar.empty()) {
        fields.emplace_back("grammar", value.grammar);
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto decode_symbols(std::span<const std::byte> value) -> result<symbol_record> {
    return decode_with_subject(std::nullopt, value);
}

auto decode_frontend_symbols(digest subject, std::span<const std::byte> value)
    -> result<symbol_record> {
    return decode_with_subject(subject, value);
}

} // namespace liquid
