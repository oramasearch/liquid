#include "liquid/resolve.hpp"

#include <cctype>
#include <format>

namespace liquid {
namespace {

auto failure(std::size_t column, std::string expected) -> error {
    return {errc::invalid_argument,
            std::format("column {}: expected {}", column, expected),
            "spec parser"};
}

auto name_character(char value) -> bool {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_' || value == '-' || value == '.';
}

class parser {
public:
    explicit parser(std::string_view input) : input_(input) {}

    auto parse() -> result<spec> {
        skip_space();
        auto output = parse_one(false);
        if (!output) {
            return std::unexpected(output.error());
        }
        skip_space();
        while (!done()) {
            if (peek() != '^') {
                return std::unexpected(failure(position_ + 1, "'^' or end of spec"));
            }
            ++position_;
            skip_space();
            auto dependency = parse_one(true);
            if (!dependency) {
                return std::unexpected(dependency.error());
            }
            output->dependencies.push_back(std::move(*dependency));
            skip_space();
        }
        return output;
    }

private:
    auto done() const -> bool {
        return position_ == input_.size();
    }

    auto peek() const -> char {
        return done() ? '\0' : input_[position_];
    }

    auto skip_space() -> void {
        while (!done() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            ++position_;
        }
    }

    auto token(std::string expected) -> result<std::string> {
        const auto begin = position_;
        while (!done() && name_character(peek())) {
            ++position_;
        }
        if (begin == position_) {
            return std::unexpected(failure(position_ + 1, std::move(expected)));
        }
        return std::string{input_.substr(begin, position_ - begin)};
    }

    auto value_token(std::string expected) -> result<std::string> {
        const auto begin = position_;
        while (!done()) {
            const auto value = peek();
            if (std::isspace(static_cast<unsigned char>(value)) != 0 || value == '+' ||
                value == '~' || value == '%' || value == '^') {
                break;
            }
            ++position_;
        }
        if (begin == position_) {
            return std::unexpected(failure(position_ + 1, std::move(expected)));
        }
        return std::string{input_.substr(begin, position_ - begin)};
    }

    auto parse_one(bool dependency) -> result<spec> {
        auto name = token("package name");
        if (!name) {
            return std::unexpected(name.error());
        }
        auto output = spec{.name = std::move(*name)};
        while (!done()) {
            skip_space();
            if (done() || peek() == '^') {
                break;
            }
            if (peek() == '@') {
                ++position_;
                auto version = value_token("version constraint after '@'");
                if (!version) {
                    return std::unexpected(version.error());
                }
                output.version = std::move(*version);
                continue;
            }
            if (peek() == '+' || peek() == '~') {
                const auto enabled = peek() == '+';
                ++position_;
                auto name_value = token("variant name");
                if (!name_value) {
                    return std::unexpected(name_value.error());
                }
                output.variants[*name_value] = enabled ? "true" : "false";
                continue;
            }
            if (peek() == '%') {
                if (dependency) {
                    return std::unexpected(
                        failure(position_ + 1, "dependency modifier or next '^'"));
                }
                ++position_;
                auto compiler_name = token("compiler name");
                if (!compiler_name) {
                    return std::unexpected(compiler_name.error());
                }
                auto compiler = compiler_spec{.name = std::move(*compiler_name)};
                if (!done() && peek() == '@') {
                    ++position_;
                    auto version = value_token("compiler version constraint after '@'");
                    if (!version) {
                        return std::unexpected(version.error());
                    }
                    compiler.version = std::move(*version);
                }
                output.compiler = std::move(compiler);
                continue;
            }
            auto key = token("variant assignment, target assignment, or modifier");
            if (!key) {
                return std::unexpected(key.error());
            }
            if (done() || peek() != '=') {
                return std::unexpected(failure(position_ + 1, "'=' after assignment name"));
            }
            ++position_;
            auto value = value_token("assignment value");
            if (!value) {
                return std::unexpected(value.error());
            }
            if (*key == "target") {
                output.target = std::move(*value);
            } else {
                output.variants[*key] = std::move(*value);
            }
        }
        return output;
    }

    std::string_view input_;
    std::size_t position_{};
};

auto append_spec(std::string& output, const spec& value, bool dependency) -> void {
    output += value.name;
    if (value.version) {
        output += '@';
        output += *value.version;
    }
    for (const auto& [name, selected] : value.variants) {
        if (selected == "true") {
            output += '+';
            output += name;
        } else if (selected == "false") {
            output += '~';
            output += name;
        }
    }
    for (const auto& [name, selected] : value.variants) {
        if (selected != "true" && selected != "false") {
            output += ' ';
            output += name;
            output += '=';
            output += selected;
        }
    }
    if (!dependency && value.compiler) {
        output += '%';
        output += value.compiler->name;
        if (value.compiler->version) {
            output += '@';
            output += *value.compiler->version;
        }
    }
    if (value.target) {
        output += " target=";
        output += *value.target;
    }
}

} // namespace

result<spec> parse_spec(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(failure(1, "package name"));
    }
    return parser{text}.parse();
}

std::string to_string(const spec& value) {
    auto output = std::string{};
    append_spec(output, value, false);
    for (const auto& dependency : value.dependencies) {
        output += " ^";
        append_spec(output, dependency, true);
    }
    return output;
}

} // namespace liquid
