#include "formats/dotenv.hpp"

#include <cctype>
#include <set>

namespace liquid::formats::dotenv {
namespace {

auto dotenv_error(std::string message, std::size_t line) -> error {
    return {errc::invalid_argument, std::move(message), ".env line " + std::to_string(line)};
}

auto trim_left(std::string_view input) -> std::string_view {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) {
        input.remove_prefix(1);
    }
    return input;
}

auto trim(std::string_view input) -> std::string_view {
    input = trim_left(input);
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) {
        input.remove_suffix(1);
    }
    return input;
}

auto valid_key(std::string_view key) -> bool {
    if (key.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(key.front());
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    for (const auto character : key.substr(1)) {
        const auto byte = static_cast<unsigned char>(character);
        if (!(std::isalnum(byte) || character == '_' || character == '.' || character == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace

auto parse(std::string_view input) -> result<document> {
    try {
        auto output = document{};
        auto keys = std::set<std::string>{};
        auto line_number = std::size_t{1};
        for (std::size_t start = 0; start <= input.size(); ++line_number) {
            auto end = input.find('\n', start);
            const auto final_line = end == std::string_view::npos;
            if (final_line) {
                end = input.size();
            }
            auto source = input.substr(start, end - start);
            if (!source.empty() && source.back() == '\r') {
                source.remove_suffix(1);
            }
            if (final_line && source.empty() && start == input.size() && !input.empty()) {
                break;
            }
            const auto significant = trim_left(source);
            if (significant.empty()) {
                output.lines.push_back({line_kind::blank, std::string{source}, {}, {}, false});
            } else if (significant.front() == '#') {
                output.lines.push_back({line_kind::comment, std::string{source}, {}, {}, false});
            } else {
                auto assignment = significant;
                auto exported = false;
                if (assignment.starts_with("export ")) {
                    exported = true;
                    assignment.remove_prefix(7);
                    assignment = trim_left(assignment);
                }
                const auto equals = assignment.find('=');
                if (equals == std::string_view::npos) {
                    return std::unexpected(dotenv_error("expected assignment", line_number));
                }
                const auto key_view = trim(assignment.substr(0, equals));
                if (!valid_key(key_view)) {
                    return std::unexpected(dotenv_error("invalid key", line_number));
                }
                auto key = std::string{key_view};
                if (!keys.insert(key).second) {
                    return std::unexpected(dotenv_error("duplicate key", line_number));
                }
                output.lines.push_back({line_kind::assignment,
                                        {},
                                        std::move(key),
                                        std::string{assignment.substr(equals + 1)},
                                        exported});
            }
            if (final_line) {
                break;
            }
            start = end + 1;
        }
        return output;
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), ".env parse"});
    }
}

auto write(const document& input) -> result<std::string> {
    try {
        auto output = std::string{};
        for (const auto& current : input.lines) {
            if (current.kind == line_kind::assignment) {
                if (!valid_key(current.key)) {
                    return std::unexpected(dotenv_error("invalid key", 0));
                }
                if (current.exported) {
                    output += "export ";
                }
                output += current.key;
                output.push_back('=');
                output += current.value;
            } else {
                output += current.text;
            }
            output.push_back('\n');
        }
        return output;
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), ".env write"});
    }
}

} // namespace liquid::formats::dotenv
