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

auto language_name(syntax_language language) -> std::string_view {
    return language == syntax_language::c ? "c" : "c++";
}

auto identifier_range(TSNode node, std::span<const std::uint8_t> source) -> source_point {
    return point(ts_node_end_byte(node), ts_node_end_point(node));
}

void append_symbol(TSNode node,
                   std::string_view kind,
                   std::string_view role,
                   std::string_view container,
                   syntax_language language,
                   std::span<const std::uint8_t> source,
                   std::vector<syntax_symbol>& output) {
    const auto text = node_text(node, source);
    if (text.empty()) {
        return;
    }
    auto symbol = std::string{language_name(language)} + ":";
    if (role == "definition" && !container.empty()) {
        symbol += std::string{container} + "::";
    }
    symbol += std::string{text};
    output.push_back(syntax_symbol{symbol,
                                   std::string{text},
                                   std::string{kind},
                                   std::string{role},
                                   point(ts_node_start_byte(node), ts_node_start_point(node)),
                                   identifier_range(node, source),
                                   std::string{container}});
}

void visit_symbols(TSNode node,
                   syntax_language language,
                   std::span<const std::uint8_t> source,
                   std::string_view container,
                   std::vector<syntax_symbol>& output) {
    const auto type = std::string_view{ts_node_type(node)};
    if (type == "function_definition") {
        auto name = ts_node_child_by_field_name(node, "declarator", std::strlen("declarator"));
        name = name_node(name);
        if (!ts_node_is_null(name)) {
            append_symbol(name, "function", "definition", container, language, source, output);
            auto nested = std::string{container};
            if (!nested.empty()) {
                nested += "::";
            }
            nested += std::string{node_text(name, source)};
            const auto count = ts_node_named_child_count(node);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto child = ts_node_named_child(node, index);
                if (ts_node_start_byte(child) == ts_node_start_byte(name) &&
                    ts_node_end_byte(child) == ts_node_end_byte(name)) {
                    continue;
                }
                visit_symbols(child, language, source, nested, output);
            }
            return;
        }
    }
    if (type == "class_specifier" || type == "struct_specifier" || type == "enum_specifier" ||
        type == "union_specifier") {
        auto name = ts_node_child_by_field_name(node, "name", std::strlen("name"));
        if (!ts_node_is_null(name)) {
            append_symbol(name, "type", "definition", container, language, source, output);
            auto nested = std::string{container};
            if (!nested.empty()) {
                nested += "::";
            }
            nested += std::string{node_text(name, source)};
            const auto count = ts_node_named_child_count(node);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto child = ts_node_named_child(node, index);
                if (ts_node_start_byte(child) != ts_node_start_byte(name) ||
                    ts_node_end_byte(child) != ts_node_end_byte(name)) {
                    visit_symbols(child, language, source, nested, output);
                }
            }
            return;
        }
    }
    if (type == "identifier" || type == "field_identifier" || type == "type_identifier") {
        append_symbol(node, "unknown", "reference", container, language, source, output);
        return;
    }
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
        visit_symbols(ts_node_named_child(node, index), language, source, container, output);
    }
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

auto parse_symbols(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_symbol> {
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
    auto output = std::vector<syntax_symbol>{};
    if (tree) {
        visit_symbols(ts_tree_root_node(tree), language, source, {}, output);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    std::ranges::sort(output, {}, [](const auto& value) { return value.start.byte; });
    return output;
}

auto encode_symbols(syntax_language language,
                    std::string_view origin,
                    const std::vector<syntax_symbol>& symbols) -> std::vector<std::uint8_t> {
    auto values = cbor_value::array{};
    values.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        auto fields = cbor_value::map{
            {"symbol", symbol.symbol},
            {"name", symbol.name},
            {"kind", symbol.kind},
            {"role", symbol.role},
            {"range",
             cbor_value::map{{"start", position_value(symbol.start)},
                             {"end", position_value(symbol.end)}}},
        };
        if (!symbol.container.empty()) {
            fields.emplace_back("container", symbol.container);
        }
        values.emplace_back(std::move(fields));
    }
    return cbor_encode(
        cbor_value{cbor_value::map{{"v", std::uint64_t{1}},
                                   {"language", std::string{language_name(language)}},
                                   {"origin", std::string{origin}},
                                   {"occurrences", std::move(values)}}});
}

} // namespace liquid_frontend
