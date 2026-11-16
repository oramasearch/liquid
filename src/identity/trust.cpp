// SPDX-License-Identifier: MIT

#include <fstream>
#include <set>
#include <string>
#include <utility>

#include "liquid/identity.hpp"

namespace liquid {

// Defined in `src/identity.cpp`: `$LIQUID_HOME`, else `$HOME/.liquid`.
auto liquid_home() -> std::filesystem::path;

namespace {

auto trust_error(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto valid_trust_name(std::string_view name) -> bool {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string_view::npos;
}

auto trim(std::string_view value) -> std::string_view {
    constexpr auto blank = std::string_view{" \t\r\n"};
    const auto first = value.find_first_not_of(blank);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(blank);
    return value.substr(first, last - first + 1);
}

} // namespace

auto trust_file_path(std::string_view name) -> std::filesystem::path {
    return liquid_home() / "trust" / std::filesystem::path{std::string{name}};
}

auto load_trusted_identities(std::string_view name) -> result<std::vector<std::string>> {
    if (!valid_trust_name(name)) {
        return std::unexpected(
            trust_error(errc::invalid_argument, "invalid trust file name", std::string{name}));
    }
    const auto path = trust_file_path(name);
    const auto missing = "no trusted identities are configured: create " + path.string() +
                         " with one TAAP identity id per line";
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(trust_error(errc::unauthorized, missing, path.string()));
    }
    auto seen = std::set<std::string>{};
    auto output = std::vector<std::string>{};
    auto line = std::string{};
    while (std::getline(input, line)) {
        auto value = std::string_view{line};
        const auto comment = value.find('#');
        if (comment != std::string_view::npos) {
            value = value.substr(0, comment);
        }
        value = trim(value);
        if (value.empty()) {
            continue;
        }
        if (value.find_first_of(" \t") != std::string_view::npos) {
            return std::unexpected(trust_error(errc::invalid_argument,
                                               "a trust file line holds more than one value",
                                               path.string()));
        }
        auto identifier = std::string{value};
        if (seen.insert(identifier).second) {
            output.push_back(std::move(identifier));
        }
    }
    if (input.bad()) {
        return std::unexpected(trust_error(errc::io, "cannot read trust file", path.string()));
    }
    if (output.empty()) {
        return std::unexpected(trust_error(errc::unauthorized, missing, path.string()));
    }
    return output;
}

} // namespace liquid
