#include "syntax.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

#include "grammar_registry.hpp"

namespace liquid_frontend {
namespace {

class parsed_tree {
public:
    parsed_tree(TSParser* parser, TSTree* tree) : parser_(parser), tree_(tree) {}

    ~parsed_tree() {
        if (tree_) {
            ts_tree_delete(tree_);
        }
        if (parser_) {
            ts_parser_delete(parser_);
        }
    }

    parsed_tree(parsed_tree&& other) noexcept
        : parser_(std::exchange(other.parser_, nullptr)),
          tree_(std::exchange(other.tree_, nullptr)) {}

    auto operator=(parsed_tree&& other) noexcept -> parsed_tree& {
        if (this == &other) {
            return *this;
        }
        if (tree_) {
            ts_tree_delete(tree_);
        }
        if (parser_) {
            ts_parser_delete(parser_);
        }
        parser_ = std::exchange(other.parser_, nullptr);
        tree_ = std::exchange(other.tree_, nullptr);
        return *this;
    }

    parsed_tree(const parsed_tree&) = delete;
    auto operator=(const parsed_tree&) -> parsed_tree& = delete;

    auto root() const -> TSNode {
        return ts_tree_root_node(tree_);
    }

private:
    TSParser* parser_{};
    TSTree* tree_{};
};

class query_handle {
public:
    explicit query_handle(TSQuery* query) : query_(query) {}
    ~query_handle() {
        if (query_) {
            ts_query_delete(query_);
        }
    }
    query_handle(const query_handle&) = delete;
    auto operator=(const query_handle&) -> query_handle& = delete;

    auto get() const -> TSQuery* {
        return query_;
    }

private:
    TSQuery* query_{};
};

class query_cursor_handle {
public:
    query_cursor_handle() : cursor_(ts_query_cursor_new()) {}
    ~query_cursor_handle() {
        if (cursor_) {
            ts_query_cursor_delete(cursor_);
        }
    }
    query_cursor_handle(const query_cursor_handle&) = delete;
    auto operator=(const query_cursor_handle&) -> query_cursor_handle& = delete;

    auto get() const -> TSQueryCursor* {
        return cursor_;
    }

private:
    TSQueryCursor* cursor_{};
};

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

auto syntax_language_name(syntax_language language) -> std::string_view {
    return language == syntax_language::c ? "c" : "c++";
}

auto parse(const grammar_descriptor& grammar, std::span<const std::uint8_t> source)
    -> std::expected<parsed_tree, syntax_error> {
    if (source.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            syntax_error{syntax_error_code::parse_failed, "source exceeds tree-sitter's limit"});
    }
    const auto* language = grammar.language_function();
    if (!language) {
        return std::unexpected(
            syntax_error{syntax_error_code::parse_failed, "grammar returned no language"});
    }
    const auto actual_abi = ts_language_abi_version(language);
    if (actual_abi != grammar.abi_version || !grammar_abi_supported(actual_abi)) {
        return std::unexpected(syntax_error{
            syntax_error_code::incompatible_abi,
            "grammar language ABI " + std::to_string(actual_abi) +
                " is incompatible with the pinned runtime",
        });
    }
    auto* parser = ts_parser_new();
    if (!parser) {
        return std::unexpected(
            syntax_error{syntax_error_code::parse_failed, "cannot allocate tree-sitter parser"});
    }
    if (!ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return std::unexpected(syntax_error{
            syntax_error_code::incompatible_abi,
            "tree-sitter rejected the grammar language ABI",
        });
    }
    auto* tree = ts_parser_parse_string(parser,
                                        nullptr,
                                        reinterpret_cast<const char*>(source.data()),
                                        static_cast<std::uint32_t>(source.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return std::unexpected(
            syntax_error{syntax_error_code::parse_failed, "tree-sitter did not produce a tree"});
    }
    return parsed_tree{parser, tree};
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

auto identifier_range(TSNode node) -> source_point {
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
    auto symbol = std::string{syntax_language_name(language)} + ":";
    if (role == "definition" && !container.empty()) {
        symbol += std::string{container} + "::";
    }
    symbol += std::string{text};
    output.push_back(syntax_symbol{symbol,
                                   std::string{text},
                                   std::string{kind},
                                   std::string{role},
                                   point(ts_node_start_byte(node), ts_node_start_point(node)),
                                   identifier_range(node),
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

struct tag_match {
    TSNode value{};
    TSNode name{};
    std::string capture;
};

auto capture_name(const TSQuery* query, std::uint32_t index) -> std::string_view {
    auto length = std::uint32_t{};
    const auto* value = ts_query_capture_name_for_id(query, index, &length);
    return value ? std::string_view{value, length} : std::string_view{};
}

auto tags(const grammar_descriptor& grammar, TSNode root)
    -> std::expected<std::vector<tag_match>, syntax_error> {
    if (grammar.tags_query.empty()) {
        return std::unexpected(syntax_error{
            syntax_error_code::unsupported,
            std::string{grammar.language} + " has no pinned tags.scm; units are unsupported",
        });
    }
    auto error_offset = std::uint32_t{};
    auto query_error = TSQueryErrorNone;
    auto* raw_query = ts_query_new(grammar.language_function(),
                                   reinterpret_cast<const char*>(grammar.tags_query.data()),
                                   static_cast<std::uint32_t>(grammar.tags_query.size()),
                                   &error_offset,
                                   &query_error);
    if (!raw_query) {
        return std::unexpected(syntax_error{
            syntax_error_code::invalid_query,
            std::string{grammar.language} + " tags.scm is incompatible at byte " +
                std::to_string(error_offset),
        });
    }
    const auto query = query_handle{raw_query};
    auto cursor = query_cursor_handle{};
    if (!cursor.get()) {
        return std::unexpected(
            syntax_error{syntax_error_code::parse_failed, "cannot allocate query cursor"});
    }
    ts_query_cursor_exec(cursor.get(), query.get(), root);
    auto output = std::vector<tag_match>{};
    auto match = TSQueryMatch{};
    while (ts_query_cursor_next_match(cursor.get(), &match)) {
        auto name = TSNode{};
        for (std::uint16_t index = 0; index < match.capture_count; ++index) {
            const auto& capture = match.captures[index];
            if (capture_name(query.get(), capture.index) == "name") {
                name = capture.node;
                break;
            }
        }
        for (std::uint16_t index = 0; index < match.capture_count; ++index) {
            const auto& capture = match.captures[index];
            const auto capture_text = capture_name(query.get(), capture.index);
            if (!capture_text.starts_with("definition.") &&
                !capture_text.starts_with("declaration.") &&
                !capture_text.starts_with("reference.")) {
                continue;
            }
            auto resolved_name = name;
            if (ts_node_is_null(resolved_name)) {
                resolved_name = name_node(capture.node);
            }
            if (!ts_node_is_null(resolved_name)) {
                output.push_back(tag_match{capture.node, resolved_name, std::string{capture_text}});
            }
        }
    }
    return output;
}

auto unit_kind(std::string_view capture) -> std::uint64_t {
    if (capture.starts_with("declaration.")) {
        return 1;
    }
    if (capture.ends_with(".class") || capture.ends_with(".enum") ||
        capture.ends_with(".interface") || capture.ends_with(".struct") ||
        capture.ends_with(".type") || capture.ends_with(".trait")) {
        return 3;
    }
    return 2;
}

auto symbol_kind_name(std::string_view capture) -> std::string_view {
    if (capture.ends_with(".function") || capture.ends_with(".method")) {
        return "function";
    }
    if (capture.ends_with(".class") || capture.ends_with(".enum") ||
        capture.ends_with(".interface") || capture.ends_with(".struct") ||
        capture.ends_with(".type") || capture.ends_with(".trait")) {
        return "type";
    }
    if (capture.ends_with(".field") || capture.ends_with(".property")) {
        return "field";
    }
    if (capture.ends_with(".module") || capture.ends_with(".namespace") ||
        capture.ends_with(".package")) {
        return "namespace";
    }
    if (capture.ends_with(".constant") || capture.ends_with(".variable")) {
        return "variable";
    }
    return "unknown";
}

auto tagged_units(const grammar_descriptor& grammar,
                  TSNode root,
                  std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_unit>, syntax_error> {
    auto matches = tags(grammar, root);
    if (!matches) {
        return std::unexpected(matches.error());
    }
    auto output = std::vector<syntax_unit>{};
    for (const auto& match : *matches) {
        if (match.capture.starts_with("reference.")) {
            continue;
        }
        const auto name = node_text(match.name, source);
        const auto start = ts_node_start_byte(match.value);
        const auto end = ts_node_end_byte(match.value);
        if (name.empty() || end > source.size() || start > end) {
            continue;
        }
        output.push_back(syntax_unit{
            .kind = unit_kind(match.capture),
            .name = std::string{name},
            .start = point(start, ts_node_start_point(match.value)),
            .end = point(end, ts_node_end_point(match.value)),
            .source = std::vector<std::uint8_t>{source.begin() + start, source.begin() + end},
        });
    }
    std::ranges::sort(output, {}, [](const auto& unit) {
        return std::tuple{unit.start.byte, unit.end.byte, unit.name, unit.kind};
    });
    output.erase(std::unique(output.begin(),
                             output.end(),
                             [](const auto& left, const auto& right) {
                                 return left.start.byte == right.start.byte &&
                                        left.end.byte == right.end.byte &&
                                        left.name == right.name && left.kind == right.kind;
                             }),
                 output.end());
    return output;
}

auto tagged_symbols(const grammar_descriptor& grammar,
                    TSNode root,
                    std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_symbol>, syntax_error> {
    auto matches = tags(grammar, root);
    if (!matches) {
        return std::unexpected(matches.error());
    }
    auto output = std::vector<syntax_symbol>{};
    for (const auto& match : *matches) {
        const auto name = node_text(match.name, source);
        if (name.empty()) {
            continue;
        }
        const auto role = match.capture.starts_with("reference.") ? "reference" : "definition";
        output.push_back(syntax_symbol{
            .symbol = std::string{grammar.language} + ":" + std::string{name},
            .name = std::string{name},
            .kind = std::string{symbol_kind_name(match.capture)},
            .role = role,
            .start = point(ts_node_start_byte(match.name), ts_node_start_point(match.name)),
            .end = identifier_range(match.name),
        });
    }
    std::ranges::sort(output, {}, [](const auto& symbol) {
        return std::tuple{symbol.start.byte, symbol.end.byte, symbol.name, symbol.role};
    });
    output.erase(std::unique(output.begin(),
                             output.end(),
                             [](const auto& left, const auto& right) {
                                 return left.start.byte == right.start.byte &&
                                        left.end.byte == right.end.byte &&
                                        left.name == right.name && left.role == right.role;
                             }),
                 output.end());
    return output;
}

auto encode_symbol_value(std::string_view language,
                         std::string_view origin,
                         std::string_view grammar_digest,
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
    auto fields = cbor_value::map{{"v", std::uint64_t{1}},
                                  {"language", std::string{language}},
                                  {"origin", std::string{origin}},
                                  {"occurrences", std::move(values)}};
    if (!grammar_digest.empty()) {
        fields.emplace_back("grammar", std::string{grammar_digest});
    }
    return cbor_encode(cbor_value{std::move(fields)});
}

} // namespace

auto find_grammar(std::string_view language) -> const grammar_descriptor* {
    const auto grammars = available_grammars();
    const auto found = std::ranges::find(grammars, language, &grammar_descriptor::language);
    return found == grammars.end() ? nullptr : &*found;
}

auto grammar_abi_supported(std::uint32_t version) -> bool {
    return version >= TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION &&
           version <= TREE_SITTER_LANGUAGE_VERSION;
}

auto parse_units(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_unit> {
    const auto* grammar = find_grammar(syntax_language_name(language));
    if (!grammar) {
        return {};
    }
    auto tree = parse(*grammar, source);
    if (!tree) {
        return {};
    }
    auto output = std::vector<syntax_unit>{};
    visit(tree->root(), source, output, false);
    std::ranges::sort(output, {}, [](const auto& unit) { return unit.start.byte; });
    return output;
}

auto parse_units(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_unit>, syntax_error> {
    const auto* grammar = find_grammar(language);
    if (!grammar) {
        return std::unexpected(syntax_error{
            syntax_error_code::unsupported,
            "language is not in the configured grammar set",
        });
    }
    auto tree = parse(*grammar, source);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    if (language == "c" || language == "c++") {
        auto output = std::vector<syntax_unit>{};
        visit(tree->root(), source, output, false);
        std::ranges::sort(output, {}, [](const auto& unit) { return unit.start.byte; });
        return output;
    }
    return tagged_units(*grammar, tree->root(), source);
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
    const auto* grammar = find_grammar(syntax_language_name(language));
    if (!grammar) {
        return {};
    }
    auto tree = parse(*grammar, source);
    if (!tree) {
        return {};
    }
    auto output = std::vector<syntax_symbol>{};
    visit_symbols(tree->root(), language, source, {}, output);
    std::ranges::sort(output, {}, [](const auto& value) { return value.start.byte; });
    return output;
}

auto parse_symbols(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_symbol>, syntax_error> {
    const auto* grammar = find_grammar(language);
    if (!grammar) {
        return std::unexpected(syntax_error{
            syntax_error_code::unsupported,
            "language is not in the configured grammar set",
        });
    }
    auto tree = parse(*grammar, source);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    if (language == "c" || language == "c++") {
        const auto syntax = language == "c" ? syntax_language::c : syntax_language::cpp;
        auto output = std::vector<syntax_symbol>{};
        visit_symbols(tree->root(), syntax, source, {}, output);
        std::ranges::sort(output, {}, [](const auto& value) { return value.start.byte; });
        return output;
    }
    return tagged_symbols(*grammar, tree->root(), source);
}

auto parse_has_error(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<bool, syntax_error> {
    const auto* grammar = find_grammar(language);
    if (!grammar) {
        return std::unexpected(syntax_error{
            syntax_error_code::unsupported,
            "language is not in the configured grammar set",
        });
    }
    return parse_has_error(*grammar, source);
}

auto parse_has_error(const grammar_descriptor& grammar, std::span<const std::uint8_t> source)
    -> std::expected<bool, syntax_error> {
    auto tree = parse(grammar, source);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    return ts_node_has_error(tree->root());
}

auto encode_symbols(syntax_language language,
                    std::string_view origin,
                    const std::vector<syntax_symbol>& symbols) -> std::vector<std::uint8_t> {
    return encode_symbol_value(syntax_language_name(language), origin, {}, symbols);
}

auto encode_symbols(std::string_view language,
                    std::string_view origin,
                    std::string_view grammar_digest,
                    const std::vector<syntax_symbol>& symbols) -> std::vector<std::uint8_t> {
    return encode_symbol_value(language, origin, grammar_digest, symbols);
}

} // namespace liquid_frontend
