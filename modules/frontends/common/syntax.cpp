#include "syntax.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_c(void);
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace liquid_frontend {
namespace {

auto node_text(TSNode node, std::span<const std::uint8_t> source) -> std::string_view {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    if (end > source.size() || start > end) {
        return {};
    }
    return {reinterpret_cast<const char*>(source.data() + start), end - start};
}

auto name_node(TSNode node) -> TSNode {
    auto best = TSNode{};
    const auto type = std::string_view{ts_node_type(node)};
    if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
        type == "operator_name" || type == "destructor_name") {
        best = node;
    }
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto nested = name_node(ts_node_named_child(node, index));
        if (!ts_node_is_null(nested)) {
            best = nested;
        }
    }
    return best;
}

auto point(std::uint32_t byte, TSPoint value) -> source_point {
    return source_point{
        .byte = byte,
        .line = static_cast<std::uint64_t>(value.row) + 1,
        .column = static_cast<std::uint64_t>(value.column) + 1,
    };
}

void append_unit(TSNode node,
                 std::uint64_t kind,
                 std::span<const std::uint8_t> source,
                 std::vector<syntax_unit>& output) {
    auto name = ts_node_child_by_field_name(node, "declarator", std::strlen("declarator"));
    if (ts_node_is_null(name)) {
        name = ts_node_child_by_field_name(node, "name", std::strlen("name"));
    }
    if (ts_node_is_null(name)) {
        name = node;
    }
    name = name_node(name);
    if (ts_node_is_null(name)) {
        return;
    }
    const auto text = node_text(name, source);
    if (text.empty()) {
        return;
    }
    const auto start_byte = ts_node_start_byte(node);
    const auto end_byte = ts_node_end_byte(node);
    const auto body = ts_node_child_by_field_name(node, "body", std::strlen("body"));
    output.push_back(syntax_unit{
        .kind = kind,
        .name = std::string{text},
        .start = point(start_byte, ts_node_start_point(node)),
        .end = point(end_byte, ts_node_end_point(node)),
        .body_start = ts_node_is_null(body) ? 0 : ts_node_start_byte(body),
        .body_end = ts_node_is_null(body) ? 0 : ts_node_end_byte(body),
        .source = std::vector<std::uint8_t>{source.begin() + start_byte, source.begin() + end_byte},
    });
}

void visit(TSNode node,
           std::span<const std::uint8_t> source,
           std::vector<syntax_unit>& output,
           bool inside_definition) {
    const auto type = std::string_view{ts_node_type(node)};
    if (type == "function_definition") {
        append_unit(node, 2, source, output);
        return;
    }
    if (!inside_definition && (type == "class_specifier" || type == "struct_specifier" ||
                               type == "enum_specifier" || type == "union_specifier")) {
        append_unit(node, 3, source, output);
    } else if (!inside_definition && (type == "declaration" || type == "type_definition")) {
        append_unit(node, 1, source, output);
        return;
    }
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
        visit(ts_node_named_child(node, index), source, output, inside_definition);
    }
}

auto position_value(const source_point& value) -> cbor_value {
    return cbor_value::map{
        {"byte", value.byte},
        {"line", value.line},
        {"column", value.column},
    };
}

} // namespace

auto parse_units(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_unit> {
    auto* parser = ts_parser_new();
    if (!parser) {
        return {};
    }
    const auto* grammar = language == syntax_language::c ? tree_sitter_c() : tree_sitter_cpp();
    if (!ts_parser_set_language(parser, grammar)) {
        ts_parser_delete(parser);
        return {};
    }
    auto* tree = ts_parser_parse_string(parser,
                                        nullptr,
                                        reinterpret_cast<const char*>(source.data()),
                                        static_cast<std::uint32_t>(source.size()));
    auto output = std::vector<syntax_unit>{};
    if (tree) {
        visit(ts_tree_root_node(tree), source, output, false);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    std::ranges::sort(output, {}, [](const auto& unit) { return unit.start.byte; });
    return output;
}

auto encode_units(const std::vector<syntax_unit>& units) -> std::vector<std::uint8_t> {
    auto values = cbor_value::array{};
    values.reserve(units.size());
    for (const auto& unit : units) {
        values.emplace_back(cbor_value::map{
            {"kind", unit.kind},
            {"name", unit.name},
            {"range",
             cbor_value::map{
                 {"start", position_value(unit.start)},
                 {"end", position_value(unit.end)},
             }},
            {"source", unit.source},
        });
    }
    return cbor_encode(cbor_value{std::move(values)});
}

} // namespace liquid_frontend
