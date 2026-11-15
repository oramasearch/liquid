// SPDX-License-Identifier: MIT

#include "liquid/repository.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

#include "edit_codec.hpp"

namespace liquid {
namespace {

struct indexed_file {
    std::string path;
    std::vector<std::byte> bytes;
    digest content;
    std::optional<symbol_record> symbols;
};

struct definition_range {
    std::uint64_t start{};
    std::uint64_t body_start{};
    std::uint64_t body_end{};
    std::uint64_t end{};
};

auto edit_error(errc code, std::string message, std::string context = {}) -> error {
    return error{code, std::move(message), std::move(context)};
}

auto string_view_of(std::span<const std::byte> bytes) -> std::string_view {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto byte_vector(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto code_path(std::string_view path) -> bool {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const auto extension = path.substr(dot);
    return extension == ".c" || extension == ".h" || extension == ".cc" || extension == ".cpp" ||
           extension == ".cxx" || extension == ".hh" || extension == ".hpp" || extension == ".hxx";
}

auto language_for(std::string_view path) -> std::string_view {
    const auto dot = path.rfind('.');
    if (dot != std::string_view::npos && path.substr(dot) == ".c") {
        return "c";
    }
    return "c++";
}

auto position_at(std::string_view source, std::uint64_t byte) -> frontend_source_position {
    byte = std::min<std::uint64_t>(byte, source.size());
    auto line = std::uint64_t{1};
    auto column = std::uint64_t{1};
    for (auto index = std::uint64_t{}; index < byte; ++index) {
        if (source[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {byte, line, column};
}

auto range_at(std::string_view source, std::uint64_t start, std::uint64_t end)
    -> frontend_source_range {
    return {position_at(source, start), position_at(source, end)};
}

auto byte_at_line(std::string_view source, std::uint64_t line) -> std::optional<std::uint64_t> {
    if (line == 0) {
        return std::nullopt;
    }
    if (line == 1) {
        return std::uint64_t{};
    }
    auto current = std::uint64_t{1};
    for (auto index = std::uint64_t{}; index < source.size(); ++index) {
        if (source[index] == '\n' && ++current == line) {
            return index + 1;
        }
    }
    if (current + 1 == line) {
        return source.size();
    }
    return std::nullopt;
}

auto node_fingerprint(std::string_view source, frontend_source_range range) -> result<digest> {
    if (range.end.byte < range.start.byte || range.end.byte > source.size()) {
        return std::unexpected(
            edit_error(errc::invalid_argument, "edit range lies outside the file"));
    }
    return hash(algorithm::blake3,
                std::span{reinterpret_cast<const std::byte*>(source.data() + range.start.byte),
                          static_cast<std::size_t>(range.end.byte - range.start.byte)});
}

auto target_for(std::string path,
                std::string kind,
                std::string_view source,
                frontend_source_range range) -> result<edit_target> {
    auto fingerprint = node_fingerprint(source, range);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return edit_target{std::move(path), {std::move(kind), range, *fingerprint}};
}

auto contains(frontend_source_range range, std::uint64_t line, std::uint64_t column) -> bool {
    const auto after_start =
        line > range.start.line || (line == range.start.line && column >= range.start.column);
    const auto before_end =
        line < range.end.line || (line == range.end.line && column < range.end.column);
    return after_start && before_end;
}

auto is_identifier_character(char value) -> bool {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

auto line_start(std::string_view source, std::uint64_t byte) -> std::uint64_t {
    while (byte > 0 && source[byte - 1] != '\n') {
        --byte;
    }
    return byte;
}

auto indentation_at(std::string_view source, std::uint64_t byte) -> std::string {
    const auto start = line_start(source, byte);
    auto end = start;
    while (end < source.size() && (source[end] == ' ' || source[end] == '\t')) {
        ++end;
    }
    return std::string{source.substr(start, end - start)};
}

auto matching_brace(std::string_view source, std::uint64_t opening)
    -> std::optional<std::uint64_t> {
    auto depth = std::uint64_t{};
    auto quote = char{};
    auto escaped = false;
    auto line_comment = false;
    auto block_comment = false;
    for (auto index = opening; index < source.size(); ++index) {
        const auto value = source[index];
        const auto next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (line_comment) {
            line_comment = value != '\n';
            continue;
        }
        if (block_comment) {
            if (value == '*' && next == '/') {
                block_comment = false;
                ++index;
            }
            continue;
        }
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == quote) {
                quote = '\0';
            }
            continue;
        }
        if (value == '/' && next == '/') {
            line_comment = true;
            ++index;
            continue;
        }
        if (value == '/' && next == '*') {
            block_comment = true;
            ++index;
            continue;
        }
        if (value == '\'' || value == '"') {
            quote = value;
            continue;
        }
        if (value == '{') {
            ++depth;
        } else if (value == '}' && --depth == 0) {
            return index;
        }
    }
    return std::nullopt;
}

auto definition_around(std::string_view source, const symbol_occurrence& occurrence)
    -> std::optional<definition_range> {
    auto opening = source.find('{', occurrence.range.end.byte);
    const auto semicolon = source.find(';', occurrence.range.end.byte);
    if (opening == std::string_view::npos ||
        (semicolon != std::string_view::npos && semicolon < opening)) {
        return std::nullopt;
    }
    auto closing = matching_brace(source, opening);
    if (!closing) {
        return std::nullopt;
    }
    auto start = line_start(source, occurrence.range.start.byte);
    while (start > 0) {
        const auto previous_end = start - 1;
        const auto previous_start = line_start(source, previous_end);
        const auto previous = source.substr(previous_start, previous_end - previous_start);
        if (!previous.starts_with("template") && !previous.starts_with("[[")) {
            break;
        }
        start = previous_start;
    }
    auto end = *closing + 1;
    if (end < source.size() && source[end] == '\r') {
        ++end;
    }
    if (end < source.size() && source[end] == '\n') {
        ++end;
    }
    return definition_range{start, opening, *closing + 1, end};
}

auto operation_target(const indexed_file& file, const symbol_occurrence& occurrence)
    -> result<edit_target> {
    return target_for(file.path,
                      occurrence.kind == symbol_kind::function    ? "function"
                      : occurrence.kind == symbol_kind::type      ? "type"
                      : occurrence.kind == symbol_kind::field     ? "field"
                      : occurrence.kind == symbol_kind::parameter ? "parameter"
                                                                  : "identifier",
                      string_view_of(file.bytes),
                      occurrence.range);
}

auto find_file(std::vector<indexed_file>& files, std::string_view path) -> indexed_file* {
    const auto found = std::ranges::find(
        files, path, [](const auto& file) { return std::string_view{file.path}; });
    return found == files.end() ? nullptr : &*found;
}

auto find_occurrence(std::vector<indexed_file>& files, const edit_location& location)
    -> std::optional<std::pair<indexed_file*, symbol_occurrence>> {
    for (auto& file : files) {
        if (!file.symbols) {
            continue;
        }
        for (const auto& occurrence : file.symbols->occurrences) {
            if (const auto* symbol = std::get_if<std::string>(&location)) {
                if (occurrence.symbol == *symbol) {
                    return std::pair{&file, occurrence};
                }
                continue;
            }
            const auto& at = std::get<edit_source_location>(location);
            if (file.path == at.path && contains(occurrence.range, at.line, at.column)) {
                return std::pair{&file, occurrence};
            }
        }
    }
    return std::nullopt;
}

auto apply_to_text(std::string_view source, std::vector<text_change> changes)
    -> result<std::string> {
    std::ranges::sort(changes, [](const auto& left, const auto& right) {
        if (left.range.start.byte != right.range.start.byte) {
            return left.range.start.byte < right.range.start.byte;
        }
        return left.range.end.byte < right.range.end.byte;
    });
    auto previous_end = std::uint64_t{};
    for (const auto& change : changes) {
        if (change.range.start.byte < previous_end ||
            change.range.end.byte < change.range.start.byte ||
            change.range.end.byte > source.size()) {
            return std::unexpected(
                edit_error(errc::invalid_argument, "text changes overlap or exceed the file"));
        }
        const auto actual =
            source.substr(change.range.start.byte, change.range.end.byte - change.range.start.byte);
        if (actual != change.old_text) {
            return std::unexpected(edit_error(
                errc::conflict, "text change no longer matches its preview", change.old_text));
        }
        previous_end = change.range.end.byte;
    }
    auto output = std::string{source};
    for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
        output.replace(change->range.start.byte,
                       change->range.end.byte - change->range.start.byte,
                       change->new_text);
    }
    return output;
}

auto make_file_edit(const indexed_file& file, std::vector<text_change> changes)
    -> result<file_edit> {
    auto after_text = apply_to_text(string_view_of(file.bytes), changes);
    if (!after_text) {
        return std::unexpected(after_text.error());
    }
    auto after = hash(algorithm::blake3, byte_vector(*after_text));
    if (!after) {
        return std::unexpected(after.error());
    }
    std::ranges::sort(changes, [](const auto& left, const auto& right) {
        if (left.range.start.byte != right.range.start.byte) {
            return left.range.start.byte < right.range.start.byte;
        }
        return left.range.end.byte < right.range.end.byte;
    });
    return file_edit{file.path, file.content, *after, std::move(changes)};
}

auto empty_target(std::string path, std::string reason_kind = "file") -> result<edit_target> {
    auto fingerprint = hash(algorithm::blake3, std::span<const std::byte>{});
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return edit_target{std::move(path),
                       {std::move(reason_kind), {{0, 1, 1}, {0, 1, 1}}, *fingerprint}};
}

auto append_non_code_refusals(const indexed_file& file,
                              std::string_view name,
                              std::vector<edit_refusal>& output) -> result<void> {
    const auto source = string_view_of(file.bytes);
    auto state = std::string_view{"code"};
    auto start = std::uint64_t{};
    auto quote = char{};
    auto escaped = false;
    for (auto index = std::uint64_t{}; index < source.size(); ++index) {
        const auto value = source[index];
        const auto next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (state == "code") {
            if (value == '/' && next == '/') {
                state = "line";
                start = index;
                ++index;
            } else if (value == '/' && next == '*') {
                state = "block";
                start = index;
                ++index;
            } else if (value == '\'' || value == '"') {
                state = "string";
                start = index;
                quote = value;
                escaped = false;
            }
            continue;
        }
        auto finished = false;
        if (state == "line") {
            finished = value == '\n';
        } else if (state == "block") {
            if (value == '*' && next == '/') {
                ++index;
                finished = true;
            }
        } else if (escaped) {
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else {
            finished = value == quote;
        }
        if (!finished && index + 1 != source.size()) {
            continue;
        }
        const auto end = index + 1;
        auto match = source.find(name, start);
        while (match != std::string_view::npos && match < end) {
            const auto before_ok = match == 0 || !is_identifier_character(source[match - 1]);
            const auto after_index = match + name.size();
            const auto after_ok =
                after_index == source.size() || !is_identifier_character(source[after_index]);
            if (before_ok && after_ok) {
                const auto range = range_at(source, match, after_index);
                auto target = target_for(file.path, "identifier", source, range);
                if (!target) {
                    return std::unexpected(target.error());
                }
                output.push_back(
                    {std::move(*target), "unresolved: occurrence is inside a string or comment"});
            }
            match = source.find(name, match + name.size());
        }
        state = "code";
    }
    return {};
}

auto preview_rename(std::vector<indexed_file>& files,
                    const rename_request& request,
                    edit_preview& preview) -> result<void> {
    static const auto identifier = std::regex{"[A-Za-z_][A-Za-z0-9_]*"};
    if (!std::regex_match(request.new_name, identifier)) {
        return std::unexpected(
            edit_error(errc::invalid_argument, "new name is not a C or C++ identifier"));
    }
    auto selected = find_occurrence(files, request.at);
    if (!selected) {
        auto path = std::holds_alternative<edit_source_location>(request.at)
                        ? std::get<edit_source_location>(request.at).path
                        : std::string{"<symbol>"};
        auto target = empty_target(std::move(path), "identifier");
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back({std::move(*target), "unresolved: symbol was not indexed"});
        return {};
    }
    auto& [selected_file, selected_occurrence] = *selected;
    auto collision_scope_file = selected_file;
    auto collision_scope = selected_occurrence;
    for (auto& file : files) {
        if (!file.symbols) {
            continue;
        }
        const auto definition =
            std::ranges::find_if(file.symbols->occurrences, [&](const auto& item) {
                return item.symbol == selected_occurrence.symbol &&
                       item.role == symbol_role::definition;
            });
        if (definition != file.symbols->occurrences.end()) {
            collision_scope_file = &file;
            collision_scope = *definition;
            break;
        }
    }
    for (const auto& file : files) {
        if (!file.symbols) {
            auto target = target_for(file.path,
                                     "file",
                                     string_view_of(file.bytes),
                                     range_at(string_view_of(file.bytes), 0, file.bytes.size()));
            if (!target) {
                return std::unexpected(target.error());
            }
            preview.refused.push_back({std::move(*target), "not indexed"});
            continue;
        }
        for (const auto& occurrence : file.symbols->occurrences) {
            const auto same_container = occurrence.container && collision_scope.container &&
                                        occurrence.container == collision_scope.container;
            const auto same_file_scope = !occurrence.container && !collision_scope.container &&
                                         file.path == collision_scope_file->path;
            if (occurrence.role == symbol_role::definition &&
                occurrence.symbol != selected_occurrence.symbol &&
                occurrence.name == request.new_name && (same_container || same_file_scope)) {
                auto target = operation_target(file, occurrence);
                if (!target) {
                    return std::unexpected(target.error());
                }
                preview.changes.clear();
                preview.refused.push_back({std::move(*target),
                                           "collision with symbol " + occurrence.symbol + " at " +
                                               file.path + ":" +
                                               std::to_string(occurrence.range.start.line) + ":" +
                                               std::to_string(occurrence.range.start.column)});
                return {};
            }
        }
    }
    for (auto& file : files) {
        if (!file.symbols) {
            continue;
        }
        auto changes = std::vector<text_change>{};
        const auto source = string_view_of(file.bytes);
        for (const auto& occurrence : file.symbols->occurrences) {
            if (occurrence.symbol != selected_occurrence.symbol) {
                continue;
            }
            if (occurrence.range.end.byte > source.size()) {
                return std::unexpected(
                    edit_error(errc::corrupt, "symbol range lies outside its blob", file.path));
            }
            const auto old_text =
                source.substr(occurrence.range.start.byte,
                              occurrence.range.end.byte - occurrence.range.start.byte);
            if (old_text != occurrence.name) {
                continue;
            }
            changes.push_back({occurrence.range, std::string{old_text}, request.new_name});
        }
        if (!changes.empty()) {
            std::ranges::sort(changes, [](const auto& left, const auto& right) {
                if (left.range.start.byte != right.range.start.byte) {
                    return left.range.start.byte < right.range.start.byte;
                }
                return left.range.end.byte < right.range.end.byte;
            });
            const auto duplicate = [](const auto& left, const auto& right) {
                return left.range.start.byte == right.range.start.byte &&
                       left.range.end.byte == right.range.end.byte;
            };
            changes.erase(std::unique(changes.begin(), changes.end(), duplicate), changes.end());
            auto edit = make_file_edit(file, std::move(changes));
            if (!edit) {
                return std::unexpected(edit.error());
            }
            preview.changes.push_back(std::move(*edit));
        }
        auto unresolved = append_non_code_refusals(file, selected_occurrence.name, preview.refused);
        if (!unresolved) {
            return std::unexpected(unresolved.error());
        }
    }
    return {};
}

auto parse_declarations(std::string_view source) -> std::map<std::string, std::string> {
    static const auto declaration = std::regex{
        R"(([A-Za-z_][A-Za-z0-9_:<>, \t*&]*?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|;|,|\)))"};
    auto output = std::map<std::string, std::string>{};
    const auto text = std::string{source};
    for (auto match = std::sregex_iterator{text.begin(), text.end(), declaration};
         match != std::sregex_iterator{};
         ++match) {
        auto type = (*match)[1].str();
        while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
            type.pop_back();
        }
        output.insert_or_assign((*match)[2].str(), std::move(type));
    }
    return output;
}

auto identifiers(std::string_view source) -> std::vector<std::string> {
    static const auto pattern = std::regex{"[A-Za-z_][A-Za-z0-9_]*"};
    const auto text = std::string{source};
    auto output = std::vector<std::string>{};
    auto seen = std::set<std::string>{};
    for (auto match = std::sregex_iterator{text.begin(), text.end(), pattern};
         match != std::sregex_iterator{};
         ++match) {
        auto name = match->str();
        if (seen.insert(name).second) {
            output.push_back(std::move(name));
        }
    }
    return output;
}

auto is_keyword(std::string_view value) -> bool {
    static const auto keywords = std::set<std::string>{
        "alignas",   "alignof",  "auto",      "bool",     "break",    "case",    "catch",
        "char",      "class",    "const",     "continue", "default",  "delete",  "do",
        "double",    "else",     "enum",      "false",    "float",    "for",     "if",
        "int",       "long",     "namespace", "new",      "noexcept", "nullptr", "private",
        "protected", "public",   "return",    "short",    "signed",   "sizeof",  "static",
        "struct",    "switch",   "template",  "this",     "throw",    "true",    "try",
        "typedef",   "typename", "union",     "unsigned", "using",    "void",    "volatile",
        "while"};
    return keywords.contains(std::string{value});
}

auto preview_extract(std::vector<indexed_file>& files,
                     const extract_request& request,
                     edit_preview& preview) -> result<void> {
    static const auto identifier = std::regex{"[A-Za-z_][A-Za-z0-9_]*"};
    if (!std::regex_match(request.name, identifier)) {
        return std::unexpected(
            edit_error(errc::invalid_argument, "extract name is not a C or C++ identifier"));
    }
    auto* file = find_file(files, request.path);
    if (!file || !file->symbols) {
        auto target = empty_target(request.path, "selection");
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back({std::move(*target), "not indexed"});
        return {};
    }
    const auto source = string_view_of(file->bytes);
    if (request.range.end.byte > source.size() ||
        request.range.start.byte >= request.range.end.byte) {
        return std::unexpected(edit_error(errc::invalid_argument, "invalid extraction range"));
    }
    const symbol_occurrence* enclosing = nullptr;
    auto unit = std::optional<definition_range>{};
    for (const auto& occurrence : file->symbols->occurrences) {
        if (occurrence.role != symbol_role::definition ||
            occurrence.kind != symbol_kind::function) {
            continue;
        }
        auto candidate = definition_around(source, occurrence);
        if (candidate && request.range.start.byte >= candidate->body_start + 1 &&
            request.range.end.byte <= candidate->body_end - 1) {
            enclosing = &occurrence;
            unit = candidate;
            break;
        }
    }
    if (!enclosing || !unit) {
        auto target = target_for(request.path, "selection", source, request.range);
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back(
            {std::move(*target), "selection is not inside one definition unit"});
        return {};
    }
    const auto selected =
        source.substr(request.range.start.byte, request.range.end.byte - request.range.start.byte);
    auto changes = std::vector<text_change>{};
    if (request.kind == extract_kind::value) {
        const auto insertion = line_start(source, request.range.start.byte);
        const auto indent = indentation_at(source, request.range.start.byte);
        const auto declaration =
            indent + "const auto " + request.name + " = " + std::string{selected} + ";\n";
        changes.push_back({range_at(source, insertion, insertion), {}, declaration});
        changes.push_back({request.range, std::string{selected}, request.name});
    } else {
        if (selected.find("return") != std::string_view::npos) {
            auto target = target_for(request.path, "selection", source, request.range);
            if (!target) {
                return std::unexpected(target.error());
            }
            preview.refused.push_back(
                {std::move(*target), "unresolved return type for extracted function"});
            return {};
        }
        const auto scope =
            source.substr(unit->body_start + 1, request.range.start.byte - unit->body_start - 1);
        auto declarations = parse_declarations(scope);
        const auto signature =
            source.substr(enclosing->range.end.byte, unit->body_start - enclosing->range.end.byte);
        auto parameters = parse_declarations(signature);
        declarations.insert(parameters.begin(), parameters.end());
        const auto local_declarations = parse_declarations(selected);
        auto parameter_names = std::vector<std::string>{};
        for (const auto& name : identifiers(selected)) {
            if (is_keyword(name) || local_declarations.contains(name)) {
                continue;
            }
            const auto found = declarations.find(name);
            if (found != declarations.end()) {
                parameter_names.push_back(name);
                continue;
            }
            const auto offset = selected.find(name);
            const auto after = offset + name.size();
            if ((after < selected.size() && selected[after] == '(') ||
                (offset > 0 && (selected[offset - 1] == '.' || selected[offset - 1] == ':'))) {
                continue;
            }
            const auto resolved =
                std::ranges::any_of(file->symbols->occurrences, [&](const auto& item) {
                    return item.name == name &&
                           (item.kind == symbol_kind::function || item.kind == symbol_kind::type);
                });
            if (!resolved) {
                auto target = target_for(request.path, "selection", source, request.range);
                if (!target) {
                    return std::unexpected(target.error());
                }
                preview.refused.push_back(
                    {std::move(*target), "unresolved type for parameter " + name});
                return {};
            }
        }
        auto parameter_declarations = std::string{};
        auto arguments = std::string{};
        for (std::size_t index = 0; index < parameter_names.size(); ++index) {
            const auto& name = parameter_names[index];
            auto type = declarations.at(name);
            if (type.find('&') == std::string::npos && type.find('*') == std::string::npos) {
                type += '&';
            }
            if (index != 0) {
                parameter_declarations += ", ";
                arguments += ", ";
            }
            parameter_declarations += type + " " + name;
            arguments += name;
        }
        auto definition = std::string{"static void "} + request.name + "(" +
                          parameter_declarations + ") {\n" + std::string{selected};
        if (!definition.ends_with('\n')) {
            definition += '\n';
        }
        definition += "}\n\n";
        const auto indent = indentation_at(source, request.range.start.byte);
        const auto call = indent + request.name + "(" + arguments + ");";
        changes.push_back({range_at(source, unit->start, unit->start), {}, definition});
        changes.push_back({request.range, std::string{selected}, call});
    }
    auto edit = make_file_edit(*file, std::move(changes));
    if (!edit) {
        return std::unexpected(edit.error());
    }
    preview.changes.push_back(std::move(*edit));
    return {};
}

auto preview_move(std::vector<indexed_file>& files,
                  const move_request& request,
                  edit_preview& preview) -> result<void> {
    auto selected = find_occurrence(files, request.at);
    if (!selected) {
        auto target = empty_target("<symbol>", "definition");
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back({std::move(*target), "unresolved: definition was not indexed"});
        return {};
    }
    auto& [from, occurrence] = *selected;
    if (occurrence.role != symbol_role::definition) {
        auto definition = std::optional<std::pair<indexed_file*, symbol_occurrence>>{};
        for (auto& file : files) {
            if (!file.symbols) {
                continue;
            }
            const auto found =
                std::ranges::find_if(file.symbols->occurrences, [&](const auto& item) {
                    return item.symbol == occurrence.symbol && item.role == symbol_role::definition;
                });
            if (found != file.symbols->occurrences.end()) {
                definition = std::pair{&file, *found};
                break;
            }
        }
        if (!definition) {
            auto target = operation_target(*from, occurrence);
            if (!target) {
                return std::unexpected(target.error());
            }
            preview.refused.push_back(
                {std::move(*target), "unresolved: symbol has no indexed definition"});
            return {};
        }
        from = definition->first;
        occurrence = std::move(definition->second);
    }
    const auto from_source = string_view_of(from->bytes);
    auto unit = definition_around(from_source, occurrence);
    if (!unit) {
        auto target = operation_target(*from, occurrence);
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back({std::move(*target), "selected symbol has no definition unit"});
        return {};
    }
    if (from->path == request.to_path) {
        auto target = operation_target(*from, occurrence);
        if (!target) {
            return std::unexpected(target.error());
        }
        preview.refused.push_back({std::move(*target), "move destination is the source file"});
        return {};
    }
    auto* to = find_file(files, request.to_path);
    auto synthetic = indexed_file{};
    if (!to) {
        synthetic.path = request.to_path;
        auto empty = hash(algorithm::blake3, std::span<const std::byte>{});
        if (!empty) {
            return std::unexpected(empty.error());
        }
        synthetic.content = *empty;
        to = &synthetic;
    }
    const auto to_source = string_view_of(to->bytes);
    auto insertion = std::uint64_t{to_source.size()};
    if (request.at_line) {
        auto selected_line = byte_at_line(to_source, *request.at_line);
        if (!selected_line) {
            return std::unexpected(
                edit_error(errc::invalid_argument, "move insertion line is outside destination"));
        }
        insertion = *selected_line;
    }
    auto moved = std::string{from_source.substr(unit->start, unit->end - unit->start)};
    if (insertion == to_source.size() && !to_source.empty() && !to_source.ends_with('\n')) {
        moved.insert(moved.begin(), '\n');
    }
    auto from_edit =
        make_file_edit(*from,
                       {{range_at(from_source, unit->start, unit->end),
                         std::string{from_source.substr(unit->start, unit->end - unit->start)},
                         {}}});
    auto to_edit =
        make_file_edit(*to, {{range_at(to_source, insertion, insertion), {}, std::move(moved)}});
    if (!from_edit) {
        return std::unexpected(from_edit.error());
    }
    if (!to_edit) {
        return std::unexpected(to_edit.error());
    }
    preview.changes.push_back(std::move(*from_edit));
    preview.changes.push_back(std::move(*to_edit));
    return {};
}

auto preview_tree_edits(std::vector<indexed_file>& files,
                        const tree_edit_request& request,
                        edit_preview& preview) -> result<void> {
    auto grouped = std::map<std::string, std::vector<text_change>>{};
    for (const auto& edit : request.edits) {
        auto* file = find_file(files, edit.target.path);
        if (!file) {
            preview.refused.push_back({edit.target, "not indexed"});
            continue;
        }
        const auto source = string_view_of(file->bytes);
        auto current = node_fingerprint(source, edit.target.node.range);
        if (!current || !same_digest(*current, edit.target.node.fingerprint)) {
            preview.refused.push_back({edit.target, "stale node fingerprint"});
            continue;
        }
        const auto& node_range = edit.target.node.range;
        const auto old_text = std::string{
            source.substr(node_range.start.byte, node_range.end.byte - node_range.start.byte)};
        if (edit.operation == tree_edit_operation::replace_node) {
            grouped[file->path].push_back({node_range, old_text, edit.text});
        } else if (edit.operation == tree_edit_operation::delete_node) {
            grouped[file->path].push_back({node_range, old_text, {}});
        } else if (edit.operation == tree_edit_operation::insert_before) {
            grouped[file->path].push_back(
                {range_at(source, node_range.start.byte, node_range.start.byte), {}, edit.text});
        } else {
            grouped[file->path].push_back(
                {range_at(source, node_range.end.byte, node_range.end.byte), {}, edit.text});
        }
    }
    for (auto& [path, changes] : grouped) {
        auto* file = find_file(files, path);
        auto planned = make_file_edit(*file, std::move(changes));
        if (!planned) {
            return std::unexpected(planned.error());
        }
        preview.changes.push_back(std::move(*planned));
    }
    return {};
}

auto collect_files(repository& source) -> runtime::task<std::vector<indexed_file>> {
    auto root = co_await source.root();
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto pending = std::vector<std::pair<std::string, liquid::tree>>{{{}, std::move(*root)}};
    auto files = std::vector<indexed_file>{};
    while (!pending.empty()) {
        auto [prefix, directory] = std::move(pending.back());
        pending.pop_back();
        for (const auto& entry : directory.entries) {
            auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
            if (entry.kind == node_kind::tree) {
                auto child = co_await source.tree(entry.id);
                if (!child) {
                    co_return std::unexpected(child.error());
                }
                pending.emplace_back(std::move(path), std::move(*child));
                continue;
            }
            if (entry.kind != node_kind::blob || !code_path(path)) {
                continue;
            }
            auto contents = co_await source.read(path);
            if (!contents) {
                co_return std::unexpected(contents.error());
            }
            auto content = hash(algorithm::blake3, *contents);
            if (!content) {
                co_return std::unexpected(content.error());
            }
            auto symbols = co_await source.symbols(*content);
            if (!symbols && symbols.error().code != errc::not_found) {
                co_return std::unexpected(symbols.error());
            }
            files.push_back({std::move(path),
                             std::move(*contents),
                             *content,
                             symbols ? std::optional{std::move(*symbols)} : std::nullopt});
        }
    }
    std::ranges::sort(files, {}, &indexed_file::path);
    co_return files;
}

} // namespace

auto edit_detail::apply_changes(const file_edit& edit, std::span<const std::byte> before)
    -> result<std::vector<std::byte>> {
    auto result = apply_to_text(string_view_of(before), edit.changes);
    if (!result) {
        return std::unexpected(result.error());
    }
    auto actual = hash(algorithm::blake3, byte_vector(*result));
    if (!actual) {
        return std::unexpected(actual.error());
    }
    if (!same_digest(*actual, edit.after)) {
        return std::unexpected(edit_error(
            errc::corrupt, "preview after digest does not match its text changes", edit.path));
    }
    return byte_vector(*result);
}

runtime::task<edit_preview> repository::preview_edit(edit_request request,
                                                     edit_attribution attribution) {
    if (attribution.tool.empty() || attribution.operation.empty()) {
        co_return std::unexpected(
            edit_error(errc::invalid_argument, "edit attribution requires tool and operation"));
    }
    auto files = co_await collect_files(*this);
    if (!files) {
        co_return std::unexpected(files.error());
    }
    auto preview = edit_preview{.attribution = std::move(attribution)};
    auto planned = std::visit(
        [&](const auto& value) -> result<void> {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, rename_request>) {
                return preview_rename(*files, value, preview);
            } else if constexpr (std::is_same_v<value_type, extract_request>) {
                return preview_extract(*files, value, preview);
            } else if constexpr (std::is_same_v<value_type, move_request>) {
                return preview_move(*files, value, preview);
            } else {
                return preview_tree_edits(*files, value, preview);
            }
        },
        request);
    if (!planned) {
        co_return std::unexpected(planned.error());
    }
    std::ranges::sort(preview.changes, {}, &file_edit::path);
    auto id = edit_detail::preview_id(preview);
    if (!id) {
        co_return std::unexpected(id.error());
    }
    preview.id = *id;
    co_return preview;
}

} // namespace liquid
