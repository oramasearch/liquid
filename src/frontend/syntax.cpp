#include "liquid/frontend.hpp"

#include <algorithm>
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

} // namespace liquid
