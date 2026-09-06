#include "liquid/frontend.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto syntax_error(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "syntax"};
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

auto decode_position(const cbor::value& value) -> result<frontend_source_position> {
    const auto* byte_value = find(value, "byte");
    const auto* line_value = find(value, "line");
    const auto* column_value = find(value, "column");
    const auto* byte = byte_value ? std::get_if<std::uint64_t>(&byte_value->data) : nullptr;
    const auto* line = line_value ? std::get_if<std::uint64_t>(&line_value->data) : nullptr;
    const auto* column = column_value ? std::get_if<std::uint64_t>(&column_value->data) : nullptr;
    if (!byte || !line || !column || *line == 0 || *column == 0) {
        return std::unexpected(syntax_error("syntax tree has an invalid position"));
    }
    return frontend_source_position{*byte, *line, *column};
}

auto decode_range(const cbor::value& value) -> result<frontend_source_range> {
    const auto* start_value = find(value, "start");
    const auto* end_value = find(value, "end");
    if (!start_value || !end_value) {
        return std::unexpected(syntax_error("syntax tree has an invalid range"));
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
        return std::unexpected(syntax_error("syntax tree has a reversed range"));
    }
    return frontend_source_range{*start, *end};
}

auto decode_node(const cbor::value& value) -> result<syntax_node> {
    const auto* kind_value = find(value, "kind");
    const auto* named_value = find(value, "named");
    const auto* range_value = find(value, "range");
    const auto* children_value = find(value, "children");
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* named = named_value ? std::get_if<bool>(&named_value->data) : nullptr;
    const auto* children =
        children_value ? std::get_if<cbor::value::array>(&children_value->data) : nullptr;
    if (!kind || kind->empty() || !named || !range_value || !children) {
        return std::unexpected(syntax_error("syntax tree has an invalid node"));
    }
    auto range = decode_range(*range_value);
    if (!range) {
        return std::unexpected(range.error());
    }
    auto output = syntax_node{};
    output.kind = *kind;
    output.named = *named;
    output.range = *range;
    if (const auto* field_value = find(value, "field")) {
        const auto* field = std::get_if<std::string>(&field_value->data);
        if (!field || field->empty()) {
            return std::unexpected(syntax_error("syntax tree has an invalid field name"));
        }
        output.field = *field;
    }
    output.children.reserve(children->size());
    for (const auto& child : *children) {
        auto decoded = decode_node(child);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (decoded->range.start.byte < output.range.start.byte ||
            decoded->range.end.byte > output.range.end.byte) {
            return std::unexpected(syntax_error("syntax child lies outside its parent"));
        }
        output.children.push_back(std::move(*decoded));
    }
    return output;
}

auto decode_compact_range(const cbor::value& value) -> result<frontend_source_range> {
    const auto* fields = std::get_if<cbor::value::array>(&value.data);
    if (!fields || fields->size() != 6) {
        return std::unexpected(syntax_error("parse tree has an invalid range"));
    }
    auto numbers = std::array<std::uint64_t, 6>{};
    for (auto index = std::size_t{}; index < numbers.size(); ++index) {
        const auto* number = std::get_if<std::uint64_t>(&fields->at(index).data);
        if (!number) {
            return std::unexpected(syntax_error("parse tree has an invalid range value"));
        }
        numbers[index] = *number;
    }
    if (numbers[3] < numbers[0] || numbers[1] == 0 || numbers[2] == 0 || numbers[4] == 0 ||
        numbers[5] == 0) {
        return std::unexpected(syntax_error("parse tree has a reversed or zero-based range"));
    }
    return frontend_source_range{{numbers[0], numbers[1], numbers[2]},
                                 {numbers[3], numbers[4], numbers[5]}};
}

auto decode_parse_node(const cbor::value& value, std::size_t kind_count) -> result<parse_node> {
    const auto* kind_value = find(value, "k");
    const auto* range_value = find(value, "r");
    const auto* error_value = find(value, "e");
    const auto* missing_value = find(value, "m");
    const auto* children_value = find(value, "c");
    const auto* kind = kind_value ? std::get_if<std::uint64_t>(&kind_value->data) : nullptr;
    const auto* error_flag = error_value ? std::get_if<bool>(&error_value->data) : nullptr;
    const auto* missing = missing_value ? std::get_if<bool>(&missing_value->data) : nullptr;
    const auto* children =
        children_value ? std::get_if<cbor::value::array>(&children_value->data) : nullptr;
    if (!kind || *kind >= kind_count || !range_value || !error_flag || !missing || !children) {
        return std::unexpected(syntax_error("parse tree has an invalid node"));
    }
    auto range = decode_compact_range(*range_value);
    if (!range) {
        return std::unexpected(range.error());
    }
    auto output = parse_node{
        .kind = static_cast<std::uint32_t>(*kind),
        .range = *range,
        .error = *error_flag,
        .missing = *missing,
    };
    if (const auto* field_value = find(value, "f")) {
        const auto* field = std::get_if<std::string>(&field_value->data);
        if (!field || field->empty()) {
            return std::unexpected(syntax_error("parse tree has an invalid field"));
        }
        output.field = *field;
    }
    output.children.reserve(children->size());
    for (const auto& child_value : *children) {
        auto child = decode_parse_node(child_value, kind_count);
        if (!child) {
            return std::unexpected(child.error());
        }
        if (child->range.start.byte < output.range.start.byte ||
            child->range.end.byte > output.range.end.byte) {
            return std::unexpected(syntax_error("parse child lies outside its parent"));
        }
        output.children.push_back(std::move(*child));
    }
    return output;
}

auto decode_parse_unit(const cbor::value& value) -> result<frontend_unit> {
    const auto* kind_value = find(value, "kind");
    const auto* name_value = find(value, "name");
    const auto* range_value = find(value, "range");
    const auto* source_value = find(value, "source");
    const auto* kind = kind_value ? std::get_if<std::uint64_t>(&kind_value->data) : nullptr;
    const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    if (!kind || *kind < 1 || *kind > 3 || !name || name->empty() || !range_value || !source) {
        return std::unexpected(syntax_error("parse tree has an invalid unit"));
    }
    auto range = decode_range(*range_value);
    if (!range) {
        return std::unexpected(range.error());
    }
    auto identity = cbor::value::map{{"kind", *kind}, {"name", *name}, {"source", *source}};
    auto encoded_identity = cbor::encode(cbor::value{std::move(identity)});
    if (!encoded_identity) {
        return std::unexpected(encoded_identity.error());
    }
    auto fingerprint = hash(algorithm::blake3, *encoded_identity);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto canonical = cbor::encode(value);
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    return frontend_unit{.kind = static_cast<frontend_unit_kind>(*kind),
                         .name = *name,
                         .range = *range,
                         .fingerprint = *fingerprint,
                         .canonical_value = std::move(*canonical)};
}

} // namespace

syntax_node::syntax_node(const syntax_node& other)
    : kind(other.kind), named(other.named), field(other.field), range(other.range),
      children(other.children) {
    relink();
}

syntax_node::syntax_node(syntax_node&& other) noexcept
    : kind(std::move(other.kind)), named(other.named), field(std::move(other.field)),
      range(other.range), children(std::move(other.children)) {
    relink();
}

auto syntax_node::operator=(const syntax_node& other) -> syntax_node& {
    if (this != &other) {
        kind = other.kind;
        named = other.named;
        field = other.field;
        range = other.range;
        children = other.children;
        relink(parent_);
    }
    return *this;
}

auto syntax_node::operator=(syntax_node&& other) noexcept -> syntax_node& {
    if (this != &other) {
        kind = std::move(other.kind);
        named = other.named;
        field = std::move(other.field);
        range = other.range;
        children = std::move(other.children);
        relink(parent_);
    }
    return *this;
}

void syntax_node::relink(syntax_node* parent) {
    parent_ = parent;
    for (auto& child : children) {
        child.relink(this);
    }
}

auto syntax_node::node_at(std::uint64_t byte) const -> const syntax_node* {
    const auto half_open =
        byte < range.end.byte || (range.start.byte == range.end.byte && byte == range.start.byte);
    if (byte < range.start.byte || !half_open) {
        return nullptr;
    }
    for (const auto& child : children) {
        const auto half_open =
            byte < child.range.end.byte ||
            (child.range.start.byte == child.range.end.byte && byte == child.range.start.byte);
        if (byte >= child.range.start.byte && half_open) {
            if (const auto* nested = child.node_at(byte)) {
                return nested;
            }
        }
    }
    return this;
}

auto syntax_node::enclosing(std::string_view node_kind) const -> const syntax_node* {
    for (auto* candidate = this; candidate; candidate = candidate->parent_) {
        if (candidate->kind == node_kind) {
            return candidate;
        }
    }
    return nullptr;
}

auto decode_syntax_tree(std::span<const std::byte> value) -> result<syntax_tree> {
    auto decoded = cbor::decode(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || canonical->size() != value.size() ||
        !std::equal(canonical->begin(), canonical->end(), value.begin())) {
        return std::unexpected(syntax_error("syntax tree is not canonical CBOR"));
    }
    const auto* version_value = find(*decoded, "v");
    const auto* language_value = find(*decoded, "language");
    const auto* root_value = find(*decoded, "root");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* language =
        language_value ? std::get_if<std::string>(&language_value->data) : nullptr;
    if (!version || *version != 1 || !language || language->empty() || !root_value) {
        return std::unexpected(syntax_error("invalid syntax tree record"));
    }
    auto root = decode_node(*root_value);
    if (!root) {
        return std::unexpected(root.error());
    }
    root->relink();
    return syntax_tree{*language, std::move(*root)};
}

auto decode_parse_tree(std::span<const std::byte> value) -> result<parse_tree> {
    auto decoded = cbor::decode(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || canonical->size() != value.size() ||
        !std::equal(canonical->begin(), canonical->end(), value.begin())) {
        return std::unexpected(syntax_error("parse tree is not canonical CBOR"));
    }
    const auto* version_value = find(*decoded, "v");
    const auto* language_value = find(*decoded, "language");
    const auto* grammar_value = find(*decoded, "grammar");
    const auto* node_types_value = find(*decoded, "node_types");
    const auto* kinds_value = find(*decoded, "kinds");
    const auto* root_value = find(*decoded, "root");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* language =
        language_value ? std::get_if<std::string>(&language_value->data) : nullptr;
    const auto* grammar = grammar_value ? std::get_if<std::string>(&grammar_value->data) : nullptr;
    const auto* node_types =
        node_types_value ? std::get_if<std::string>(&node_types_value->data) : nullptr;
    const auto* kinds = kinds_value ? std::get_if<cbor::value::array>(&kinds_value->data) : nullptr;
    if (!version || *version != 1 || !language || language->empty() || !grammar ||
        grammar->empty() || !node_types || node_types->empty() || !kinds || kinds->empty() ||
        !root_value) {
        return std::unexpected(syntax_error("invalid parse tree record"));
    }
    auto output = parse_tree{.language = *language, .grammar = *grammar, .node_types = *node_types};
    output.node_kinds.reserve(kinds->size());
    for (const auto& kind_value : *kinds) {
        const auto* kind = std::get_if<std::string>(&kind_value.data);
        if (!kind) {
            return std::unexpected(syntax_error("parse tree has an invalid node kind table"));
        }
        output.node_kinds.push_back(*kind);
    }
    auto root = decode_parse_node(*root_value, output.node_kinds.size());
    if (!root) {
        return std::unexpected(root.error());
    }
    output.root = std::move(*root);
    if (const auto* units_value = find(*decoded, "units")) {
        const auto* unit_bytes = std::get_if<cbor::value::bytes>(&units_value->data);
        if (!unit_bytes) {
            return std::unexpected(syntax_error("parse tree has an invalid units sidecar"));
        }
        auto unit_value = cbor::decode(*unit_bytes);
        const auto* units =
            unit_value ? std::get_if<cbor::value::array>(&unit_value->data) : nullptr;
        if (!units) {
            return std::unexpected(syntax_error("parse tree units are not an array"));
        }
        output.units.reserve(units->size());
        for (const auto& value : *units) {
            auto unit = decode_parse_unit(value);
            if (!unit) {
                return std::unexpected(unit.error());
            }
            output.units.push_back(std::move(*unit));
        }
    }
    return output;
}

} // namespace liquid
