#include "formats/json.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

namespace liquid::formats::json {
namespace {

auto json_error(std::string message, std::size_t offset) -> error {
    return {errc::invalid_argument, std::move(message), "JSON byte " + std::to_string(offset)};
}

auto append_utf8(std::string& output, std::uint32_t codepoint) -> bool {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
        return true;
    }
    if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
        return false;
    }
    if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        return true;
    }
    return false;
}

auto valid_utf8(std::string_view input) -> bool {
    for (std::size_t offset = 0; offset < input.size();) {
        const auto first = static_cast<unsigned char>(input[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        auto continuation = std::size_t{};
        auto codepoint = std::uint32_t{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (input.size() - offset <= continuation) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation; ++index) {
            const auto next = static_cast<unsigned char>(input[offset + index]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        constexpr auto minimum = std::array<std::uint32_t, 4>{0, 0x80, 0x800, 0x10000};
        if (codepoint < minimum[continuation] || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        offset += continuation + 1;
    }
    return true;
}

class parser {
public:
    explicit parser(std::string_view input) : input_(input) {}

    auto document() -> result<value> {
        whitespace();
        auto output = item();
        if (!output) {
            return output;
        }
        whitespace();
        if (position_ != input_.size()) {
            return std::unexpected(json_error("trailing input", position_));
        }
        return output;
    }

private:
    auto item() -> result<value> {
        if (position_ == input_.size()) {
            return std::unexpected(json_error("unexpected end of input", position_));
        }
        switch (input_[position_]) {
        case 'n':
            return literal("null", value{null{}});
        case 't':
            return literal("true", value{true});
        case 'f':
            return literal("false", value{false});
        case '"': {
            auto text = string();
            if (!text) {
                return std::unexpected(text.error());
            }
            return value{std::move(*text)};
        }
        case '[':
            return array();
        case '{':
            return object();
        default:
            return numeric();
        }
    }

    auto literal(std::string_view expected, value output) -> result<value> {
        if (!input_.substr(position_).starts_with(expected)) {
            return std::unexpected(json_error("invalid literal", position_));
        }
        position_ += expected.size();
        return output;
    }

    auto hex_quad() -> result<std::uint32_t> {
        if (input_.size() - position_ < 4) {
            return std::unexpected(json_error("truncated Unicode escape", position_));
        }
        auto output = std::uint32_t{};
        for (std::size_t index = 0; index < 4; ++index) {
            const auto character = input_[position_++];
            output <<= 4U;
            if (character >= '0' && character <= '9') {
                output |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                output |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                output |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return std::unexpected(json_error("invalid Unicode escape", position_ - 1));
            }
        }
        return output;
    }

    auto unicode_escape(std::string& output) -> result<void> {
        auto first = hex_quad();
        if (!first) {
            return std::unexpected(first.error());
        }
        auto codepoint = *first;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
                return std::unexpected(json_error("missing low surrogate", position_));
            }
            position_ += 2;
            auto second = hex_quad();
            if (!second) {
                return std::unexpected(second.error());
            }
            if (*second < 0xdc00 || *second > 0xdfff) {
                return std::unexpected(json_error("invalid low surrogate", position_ - 4));
            }
            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (*second - 0xdc00U);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            return std::unexpected(json_error("unpaired low surrogate", position_ - 4));
        }
        if (!append_utf8(output, codepoint)) {
            return std::unexpected(json_error("invalid Unicode code point", position_));
        }
        return {};
    }

    auto string() -> result<std::string> {
        const auto start = position_;
        ++position_;
        auto output = std::string{};
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return output;
            }
            if (character < 0x20U) {
                return std::unexpected(json_error("unescaped control character", position_ - 1));
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size()) {
                return std::unexpected(json_error("truncated escape", position_));
            }
            const auto escaped = input_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                auto appended = unicode_escape(output);
                if (!appended) {
                    return std::unexpected(appended.error());
                }
                break;
            }
            default:
                return std::unexpected(json_error("invalid escape", position_ - 1));
            }
        }
        return std::unexpected(json_error("unterminated string", start));
    }

    auto numeric() -> result<value> {
        const auto start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ == input_.size()) {
            return std::unexpected(json_error("truncated number", start));
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return std::unexpected(json_error("leading zero in number", position_));
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        } else {
            return std::unexpected(json_error("invalid number", start));
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const auto fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction) {
                return std::unexpected(json_error("missing fractional digits", position_));
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const auto exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent) {
                return std::unexpected(json_error("missing exponent digits", position_));
            }
        }
        return value{number{std::string{input_.substr(start, position_ - start)}}};
    }

    auto array() -> result<value> {
        ++position_;
        whitespace();
        auto output = value::array{};
        if (consume(']')) {
            return value{std::move(output)};
        }
        while (true) {
            auto child = item();
            if (!child) {
                return std::unexpected(child.error());
            }
            output.push_back(std::move(*child));
            whitespace();
            if (consume(']')) {
                return value{std::move(output)};
            }
            if (!consume(',')) {
                return std::unexpected(json_error("expected comma or closing bracket", position_));
            }
            whitespace();
        }
    }

    auto object() -> result<value> {
        ++position_;
        whitespace();
        auto output = value::object{};
        if (consume('}')) {
            return value{std::move(output)};
        }
        while (true) {
            if (position_ == input_.size() || input_[position_] != '"') {
                return std::unexpected(json_error("expected object key", position_));
            }
            auto key = string();
            if (!key) {
                return std::unexpected(key.error());
            }
            for (const auto& [existing, ignored] : output) {
                static_cast<void>(ignored);
                if (existing == *key) {
                    return std::unexpected(json_error("duplicate object key", position_));
                }
            }
            whitespace();
            if (!consume(':')) {
                return std::unexpected(json_error("expected colon", position_));
            }
            whitespace();
            auto child = item();
            if (!child) {
                return std::unexpected(child.error());
            }
            output.emplace_back(std::move(*key), std::move(*child));
            whitespace();
            if (consume('}')) {
                return value{std::move(output)};
            }
            if (!consume(',')) {
                return std::unexpected(json_error("expected comma or closing brace", position_));
            }
            whitespace();
        }
    }

    void whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\n' || input_[position_] == '\r')) {
            ++position_;
        }
    }

    auto consume(char expected) -> bool {
        if (position_ == input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    std::string_view input_;
    std::size_t position_{};
};

void indent(std::string& output, std::size_t depth) {
    output.append(depth * 2, ' ');
}

void write_string(std::string_view input, std::string& output) {
    constexpr auto hex = std::string_view{"0123456789abcdef"};
    output.push_back('"');
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
}

void write_value(const value& input, std::string& output, std::size_t depth) {
    std::visit(
        [&](const auto& item) {
            using type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<type, null>) {
                output += "null";
            } else if constexpr (std::is_same_v<type, bool>) {
                output += item ? "true" : "false";
            } else if constexpr (std::is_same_v<type, number>) {
                output += item.text;
            } else if constexpr (std::is_same_v<type, std::string>) {
                write_string(item, output);
            } else if constexpr (std::is_same_v<type, value::array>) {
                if (item.empty()) {
                    output += "[]";
                    return;
                }
                output += "[\n";
                for (std::size_t index = 0; index < item.size(); ++index) {
                    indent(output, depth + 1);
                    write_value(item[index], output, depth + 1);
                    output += index + 1 == item.size() ? "\n" : ",\n";
                }
                indent(output, depth);
                output.push_back(']');
            } else if constexpr (std::is_same_v<type, value::object>) {
                if (item.empty()) {
                    output += "{}";
                    return;
                }
                output += "{\n";
                for (std::size_t index = 0; index < item.size(); ++index) {
                    indent(output, depth + 1);
                    write_string(item[index].first, output);
                    output += ": ";
                    write_value(item[index].second, output, depth + 1);
                    output += index + 1 == item.size() ? "\n" : ",\n";
                }
                indent(output, depth);
                output.push_back('}');
            }
        },
        input.data);
}

} // namespace

auto parse(std::string_view input) -> result<value> {
    try {
        if (!valid_utf8(input)) {
            return std::unexpected(json_error("invalid UTF-8", 0));
        }
        return parser{input}.document();
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), "JSON parse"});
    }
}

auto write(const value& input) -> result<std::string> {
    try {
        auto output = std::string{};
        write_value(input, output, 0);
        output.push_back('\n');
        return output;
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), "JSON write"});
    }
}

} // namespace liquid::formats::json
