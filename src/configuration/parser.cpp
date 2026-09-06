#include "liquid/configuration.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace liquid {
namespace {

enum class token_kind : std::uint8_t {
    end,
    word,
    string,
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    comma,
    semicolon,
    colon,
    equal,
};

struct token {
    token_kind kind{token_kind::end};
    std::string text;
    configuration_location location;
};

auto parse_failure(const configuration_location& location,
                   std::string expected,
                   std::string found,
                   std::string hint) -> error {
    auto message = std::format("{}:{}:{}: expected {}, found {}",
                               location.file.generic_string(),
                               location.line,
                               location.column,
                               expected,
                               found);
    if (!hint.empty()) {
        message += "\n  hint: " + hint;
    }
    return {errc::invalid_argument, std::move(message), "configuration parser"};
}

auto printable(char value) -> std::string {
    if (value == '\0') {
        return "end of file";
    }
    return std::format("'{}'", value);
}

class lexer {
public:
    lexer(std::string_view input, std::filesystem::path file)
        : input_(input), file_(std::move(file)) {}

    auto next() -> result<token> {
        skip_space_and_comments();
        const auto location = configuration_location{file_, line_, column_};
        if (done()) {
            return token{token_kind::end, {}, location};
        }
        const auto value = peek();
        switch (value) {
        case '{':
            advance();
            return token{token_kind::left_brace, "{", location};
        case '}':
            advance();
            return token{token_kind::right_brace, "}", location};
        case '[':
            advance();
            return token{token_kind::left_bracket, "[", location};
        case ']':
            advance();
            return token{token_kind::right_bracket, "]", location};
        case ',':
            advance();
            return token{token_kind::comma, ",", location};
        case ';':
            advance();
            return token{token_kind::semicolon, ";", location};
        case ':':
            advance();
            return token{token_kind::colon, ":", location};
        case '=':
            advance();
            return token{token_kind::equal, "=", location};
        case '"':
            return string_token(location);
        default:
            return word_token(location);
        }
    }

private:
    auto done() const -> bool {
        return position_ == input_.size();
    }

    auto peek(std::size_t offset = 0) const -> char {
        return position_ + offset < input_.size() ? input_[position_ + offset] : '\0';
    }

    auto advance() -> char {
        const auto value = input_[position_++];
        if (value == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return value;
    }

    auto skip_space_and_comments() -> void {
        for (;;) {
            while (!done() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
            if (peek() == '#' || (peek() == '/' && peek(1) == '/')) {
                while (!done() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            return;
        }
    }

    auto string_token(configuration_location location) -> result<token> {
        advance();
        auto output = std::string{};
        while (!done() && peek() != '"') {
            const auto value = advance();
            if (value != '\\') {
                output.push_back(value);
                continue;
            }
            if (done()) {
                return std::unexpected(parse_failure(
                    location, "string escape", "end of file", "close the quoted string"));
            }
            switch (advance()) {
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '"':
                output.push_back('"');
                break;
            default:
                return std::unexpected(parse_failure(location,
                                                     "valid string escape",
                                                     "unknown escape",
                                                     "use \\, \", \\n, \\r, or \\t"));
            }
        }
        if (done()) {
            return std::unexpected(
                parse_failure(location, "closing quote", "end of file", "close the quoted string"));
        }
        advance();
        return token{token_kind::string, std::move(output), std::move(location)};
    }

    auto word_token(configuration_location location) -> result<token> {
        const auto begin = position_;
        const auto digest_prefix = input_.substr(position_).starts_with("blake3:") ||
                                   input_.substr(position_).starts_with("sha256:") ||
                                   input_.substr(position_).starts_with("blake3k:");
        while (!done()) {
            const auto value = peek();
            if (std::isspace(static_cast<unsigned char>(value)) != 0 || value == '{' ||
                value == '}' || value == '[' || value == ']' || value == ',' || value == ';' ||
                value == '=' || value == '"' || (!digest_prefix && value == ':')) {
                break;
            }
            advance();
        }
        if (begin == position_) {
            return std::unexpected(parse_failure(
                location, "token", printable(peek()), "remove the unexpected character"));
        }
        return token{token_kind::word,
                     std::string{input_.substr(begin, position_ - begin)},
                     std::move(location)};
    }

    std::string_view input_;
    std::filesystem::path file_;
    std::size_t position_{};
    std::uint64_t line_{1};
    std::uint64_t column_{1};
};

class parser {
public:
    parser(std::string_view input, std::filesystem::path origin)
        : lexer_(input, origin), origin_(std::move(origin)) {}

    auto parse() -> result<configuration_document> {
        auto loaded = advance();
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        auto output = configuration_document{.origin = origin_};
        while (current_.kind != token_kind::end) {
            auto record = parse_statement();
            if (!record) {
                return std::unexpected(record.error());
            }
            output.records.push_back(std::move(*record));
            auto separated = skip_separator();
            if (!separated) {
                return std::unexpected(separated.error());
            }
        }
        return output;
    }

private:
    auto advance() -> result<void> {
        auto next = lexer_.next();
        if (!next) {
            return std::unexpected(next.error());
        }
        current_ = std::move(*next);
        return {};
    }

    auto consume(token_kind kind, std::string expected, std::string hint = {}) -> result<token> {
        if (current_.kind != kind) {
            const auto found = current_.kind == token_kind::end
                                   ? std::string{"end of file"}
                                   : std::format("'{}'", current_.text);
            return std::unexpected(
                parse_failure(current_.location, std::move(expected), found, std::move(hint)));
        }
        auto output = current_;
        auto moved = advance();
        if (!moved) {
            return std::unexpected(moved.error());
        }
        return output;
    }

    auto skip_separator() -> result<void> {
        while (current_.kind == token_kind::comma || current_.kind == token_kind::semicolon) {
            auto moved = advance();
            if (!moved) {
                return std::unexpected(moved.error());
            }
        }
        return {};
    }

    auto parse_statement() -> result<configuration_record> {
        auto kind = consume(token_kind::word, "record name", "use package, build, or source");
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (kind->text == "include") {
            auto path = consume(token_kind::string,
                                "relative include path",
                                "includes use include \"relative/path.liquid\"");
            if (!path) {
                return std::unexpected(path.error());
            }
            return configuration_record{
                .kind = "include",
                .location = kind->location,
                .fields = {{"path",
                            path->location,
                            configuration_value{.kind = configuration_value_kind::path,
                                                .location = path->location,
                                                .text = std::move(path->text)}}}};
        }
        if (kind->text == "when") {
            return parse_when(kind->location);
        }
        if (kind->text == "generate" && current_.kind == token_kind::equal) {
            auto ignored = advance();
            if (!ignored) {
                return std::unexpected(ignored.error());
            }
            auto build = consume(token_kind::word,
                                 "build after generate =",
                                 "generators use generate = build { ... }");
            if (!build) {
                return std::unexpected(build.error());
            }
            if (build->text != "build") {
                return std::unexpected(parse_failure(build->location,
                                                     "build",
                                                     "'" + build->text + "'",
                                                     "generators are ordinary build actions"));
            }
            return parse_record_body("generate", kind->location, std::nullopt);
        }
        return parse_record_after_kind(*kind);
    }

    auto parse_record_after_kind(const token& kind) -> result<configuration_record> {
        auto name = std::optional<std::string>{};
        if (current_.kind == token_kind::word || current_.kind == token_kind::string) {
            name = current_.text;
            auto moved = advance();
            if (!moved) {
                return std::unexpected(moved.error());
            }
        }
        return parse_record_body(kind.text, kind.location, std::move(name));
    }

    auto parse_record_body(std::string kind,
                           configuration_location location,
                           std::optional<std::string> name) -> result<configuration_record> {
        auto opening =
            consume(token_kind::left_brace, "'{'", "records place their fields inside braces");
        if (!opening) {
            return std::unexpected(opening.error());
        }
        auto output = configuration_record{
            .kind = std::move(kind), .name = std::move(name), .location = std::move(location)};
        while (current_.kind != token_kind::right_brace) {
            if (current_.kind == token_kind::end) {
                return std::unexpected(
                    parse_failure(current_.location, "'}'", "end of file", "close the record"));
            }
            auto item = consume(
                token_kind::word, "field or nested record", "field names are unquoted identifiers");
            if (!item) {
                return std::unexpected(item.error());
            }
            if (item->text == "when") {
                auto nested = parse_when(item->location);
                if (!nested) {
                    return std::unexpected(nested.error());
                }
                output.records.push_back(std::move(*nested));
            } else if (current_.kind == token_kind::colon || current_.kind == token_kind::equal) {
                auto ignored = advance();
                if (!ignored) {
                    return std::unexpected(ignored.error());
                }
                auto value = parse_value();
                if (!value) {
                    return std::unexpected(value.error());
                }
                output.fields.push_back({std::move(item->text), item->location, std::move(*value)});
            } else {
                auto nested = parse_record_after_kind(*item);
                if (!nested) {
                    return std::unexpected(nested.error());
                }
                output.records.push_back(std::move(*nested));
            }
            auto separated = skip_separator();
            if (!separated) {
                return std::unexpected(separated.error());
            }
        }
        auto closing = consume(token_kind::right_brace, "'}'");
        if (!closing) {
            return std::unexpected(closing.error());
        }
        return output;
    }

    auto parse_when(configuration_location location) -> result<configuration_record> {
        auto parts = std::vector<std::string>{};
        while (current_.kind != token_kind::left_brace && current_.kind != token_kind::end) {
            parts.push_back(current_.text);
            auto moved = advance();
            if (!moved) {
                return std::unexpected(moved.error());
            }
        }
        if (parts.empty()) {
            return std::unexpected(parse_failure(location,
                                                 "spec constraint after when",
                                                 "'{'",
                                                 "for example: when \"hdf5 +mpi\" { ... }"));
        }
        auto constraint = std::string{};
        for (const auto& part : parts) {
            if (!constraint.empty()) {
                constraint += ' ';
            }
            constraint += part;
        }
        auto parsed = parse_spec(constraint);
        if (!parsed) {
            return std::unexpected(parse_failure(
                location, "spec constraint", "'" + constraint + "'", parsed.error().message));
        }
        auto output = parse_record_body("when", location, std::nullopt);
        if (!output) {
            return std::unexpected(output.error());
        }
        output->condition = std::move(*parsed);
        output->condition_text = std::move(constraint);
        return output;
    }

    auto parse_value() -> result<configuration_value> {
        const auto location = current_.location;
        if (current_.kind == token_kind::string) {
            auto value = current_.text;
            auto moved = advance();
            if (!moved) {
                return std::unexpected(moved.error());
            }
            return configuration_value{.kind = configuration_value_kind::string,
                                       .location = location,
                                       .text = std::move(value)};
        }
        if (current_.kind == token_kind::left_bracket) {
            return parse_list();
        }
        if (current_.kind == token_kind::left_brace) {
            return parse_map();
        }
        auto value =
            consume(token_kind::word,
                    "literal",
                    "use a string, integer, boolean, version, spec, digest, path, list, or map");
        if (!value) {
            return std::unexpected(value.error());
        }
        if (value->text == "true" || value->text == "false") {
            return configuration_value{.kind = configuration_value_kind::boolean,
                                       .location = location,
                                       .boolean = value->text == "true"};
        }
        if (value->text == "spec" || value->text == "path") {
            auto body = consume(token_kind::string,
                                value->text == "spec" ? "quoted spec" : "quoted path",
                                value->text == "spec" ? "use spec \"hdf5 @1.14\""
                                                      : "use path \"build/output\"");
            if (!body) {
                return std::unexpected(body.error());
            }
            if (value->text == "spec") {
                auto parsed = parse_spec(body->text);
                if (!parsed) {
                    return std::unexpected(parse_failure(
                        body->location, "spec", "'" + body->text + "'", parsed.error().message));
                }
            }
            return configuration_value{.kind = value->text == "spec"
                                                   ? configuration_value_kind::spec
                                                   : configuration_value_kind::path,
                                       .location = location,
                                       .text = std::move(body->text)};
        }
        if (value->text.starts_with("blake3:") || value->text.starts_with("sha256:") ||
            value->text.starts_with("blake3k:")) {
            auto parsed = parse_digest(value->text);
            if (!parsed) {
                return std::unexpected(
                    parse_failure(location,
                                  "digest",
                                  "'" + value->text + "'",
                                  "digests contain an algorithm and 64 hexadecimal digits"));
            }
            return configuration_value{.kind = configuration_value_kind::digest,
                                       .location = location,
                                       .text = std::move(value->text)};
        }
        auto integer_text = std::string_view{value->text};
        if (integer_text.starts_with('-')) {
            integer_text.remove_prefix(1);
        }
        const auto integer =
            !integer_text.empty() && std::ranges::all_of(integer_text, [](char item) {
                return std::isdigit(static_cast<unsigned char>(item)) != 0;
            });
        if (integer) {
            try {
                return configuration_value{.kind = configuration_value_kind::integer,
                                           .location = location,
                                           .integer = std::stoll(value->text)};
            } catch (...) {
                return std::unexpected(parse_failure(
                    location, "integer", "out-of-range integer", "use a signed 64-bit integer"));
            }
        }
        const auto version_punctuation = value->text.find_first_of(".-+");
        if (version_punctuation != std::string::npos &&
            std::isdigit(static_cast<unsigned char>(value->text.front())) != 0 &&
            std::ranges::all_of(value->text, [](char item) {
                return std::isdigit(static_cast<unsigned char>(item)) != 0 || item == '.' ||
                       item == '-' || item == '+';
            })) {
            return configuration_value{.kind = configuration_value_kind::version,
                                       .location = location,
                                       .text = std::move(value->text)};
        }
        return configuration_value{.kind = configuration_value_kind::identifier,
                                   .location = location,
                                   .text = std::move(value->text)};
    }

    auto parse_list() -> result<configuration_value> {
        const auto location = current_.location;
        auto opening = consume(token_kind::left_bracket, "'['");
        if (!opening) {
            return std::unexpected(opening.error());
        }
        auto output =
            configuration_value{.kind = configuration_value_kind::list, .location = location};
        while (current_.kind != token_kind::right_bracket) {
            auto value = parse_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            output.list.push_back(std::move(*value));
            if (current_.kind == token_kind::comma || current_.kind == token_kind::semicolon) {
                auto separated = skip_separator();
                if (!separated) {
                    return std::unexpected(separated.error());
                }
            } else if (current_.kind != token_kind::right_bracket) {
                return std::unexpected(parse_failure(current_.location,
                                                     "',' or ']'",
                                                     "'" + current_.text + "'",
                                                     "separate list elements with commas"));
            }
        }
        auto closing = consume(token_kind::right_bracket, "']'");
        if (!closing) {
            return std::unexpected(closing.error());
        }
        return output;
    }

    auto parse_map() -> result<configuration_value> {
        const auto location = current_.location;
        auto opening = consume(token_kind::left_brace, "'{' after map field");
        if (!opening) {
            return std::unexpected(opening.error());
        }
        auto output =
            configuration_value{.kind = configuration_value_kind::map, .location = location};
        while (current_.kind != token_kind::right_brace) {
            if (current_.kind != token_kind::word && current_.kind != token_kind::string) {
                return std::unexpected(parse_failure(current_.location,
                                                     "map key",
                                                     "'" + current_.text + "'",
                                                     "map keys are identifiers or strings"));
            }
            const auto key = current_;
            auto moved = advance();
            if (!moved) {
                return std::unexpected(moved.error());
            }
            auto colon = consume(token_kind::colon, "':' after map key");
            if (!colon) {
                return std::unexpected(colon.error());
            }
            auto value = parse_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            if (!output.map.emplace(key.text, std::move(*value)).second) {
                return std::unexpected(parse_failure(key.location,
                                                     "unique map key",
                                                     "duplicate '" + key.text + "'",
                                                     "remove the duplicate map entry"));
            }
            if (current_.kind == token_kind::comma || current_.kind == token_kind::semicolon) {
                auto separated = skip_separator();
                if (!separated) {
                    return std::unexpected(separated.error());
                }
            } else if (current_.kind != token_kind::right_brace) {
                return std::unexpected(parse_failure(current_.location,
                                                     "',' or '}'",
                                                     "'" + current_.text + "'",
                                                     "separate map entries with commas"));
            }
        }
        auto closing = consume(token_kind::right_brace, "'}' after map");
        if (!closing) {
            return std::unexpected(closing.error());
        }
        return output;
    }

    lexer lexer_;
    std::filesystem::path origin_;
    token current_;
};

auto repository_root(std::filesystem::path origin) -> std::filesystem::path {
    auto current = origin.parent_path();
    auto code = std::error_code{};
    current = std::filesystem::weakly_canonical(current, code);
    if (code) {
        return origin.parent_path().lexically_normal();
    }
    auto candidate = current;
    while (!candidate.empty()) {
        if (std::filesystem::exists(candidate / ".git") ||
            std::filesystem::exists(candidate / ".liquid")) {
            return candidate;
        }
        if (candidate == candidate.root_path()) {
            break;
        }
        candidate = candidate.parent_path();
    }
    return current;
}

auto within(const std::filesystem::path& root, const std::filesystem::path& candidate) -> bool {
    const auto relative = candidate.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

auto expand_includes(configuration_document& document,
                     const std::filesystem::path& root,
                     std::set<std::filesystem::path>& active) -> result<void> {
    for (auto& record : document.records) {
        if (record.kind != "include") {
            continue;
        }
        const auto& field = record.fields.front();
        const auto requested = std::filesystem::path{field.value.text};
        const auto parent_reference = std::ranges::find(requested, "..") != requested.end();
        if (requested.is_absolute() || parent_reference ||
            field.value.text.find("://") != std::string::npos) {
            return std::unexpected(
                parse_failure(field.value.location,
                              "repository-relative include",
                              "path outside repository",
                              "remote and parent-directory includes are forbidden"));
        }
        auto path = (record.location.file.parent_path() / requested).lexically_normal();
        auto code = std::error_code{};
        path = std::filesystem::weakly_canonical(path, code);
        if (code || !within(root, path)) {
            return std::unexpected(
                parse_failure(field.value.location,
                              "repository-relative include",
                              "path outside repository",
                              "remote and parent-directory includes are forbidden"));
        }
        if (path.extension() != ".liquid") {
            return std::unexpected(parse_failure(field.value.location,
                                                 ".liquid include",
                                                 "'" + path.extension().string() + "'",
                                                 "configuration files use the .liquid extension"));
        }
        if (!active.insert(path).second) {
            return std::unexpected(parse_failure(field.value.location,
                                                 "acyclic include",
                                                 "include cycle",
                                                 "remove the recursive include"));
        }
        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream) {
            return std::unexpected(parse_failure(
                field.value.location, "readable include", "missing file", path.generic_string()));
        }
        const auto text = std::string{std::istreambuf_iterator<char>{stream}, {}};
        auto included = parser{text, path}.parse();
        if (!included) {
            return std::unexpected(included.error());
        }
        auto expanded = expand_includes(*included, root, active);
        if (!expanded) {
            return std::unexpected(expanded.error());
        }
        record.records = std::move(included->records);
        active.erase(path);
    }
    return {};
}

} // namespace

result<configuration_document> parse_configuration(std::string_view text,
                                                   std::filesystem::path origin) {
    if (origin.empty()) {
        origin = "<memory>.liquid";
    }
    auto document = parser{text, origin}.parse();
    if (!document) {
        return std::unexpected(document.error());
    }
    auto active = std::set<std::filesystem::path>{};
    auto code = std::error_code{};
    auto absolute = std::filesystem::weakly_canonical(origin, code);
    if (!code) {
        active.insert(absolute);
    }
    auto expanded = expand_includes(*document, repository_root(origin), active);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    return document;
}

} // namespace liquid
