// SPDX-License-Identifier: MIT

#include "cli/metadata.hpp"

#include <algorithm>
#include <format>
#include <string_view>

namespace liquid_cli {
namespace {

auto find_flag(const command& definition, std::string_view value) -> const flag* {
    const auto found = std::ranges::find(definition.flags, value, &flag::name);
    return found == definition.flags.end() ? nullptr : &*found;
}

auto validate_range(const command& definition, int argc, char** argv, bool stop_at_positional)
    -> std::string {
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--") {
            return {};
        }
        if (!argument.starts_with("--")) {
            if (stop_at_positional) {
                return {};
            }
            continue;
        }
        const auto* definition_flag = find_flag(definition, argument);
        if (definition_flag == nullptr) {
            return std::format("{} does not accept {}", definition.name, argument);
        }
        if (!definition_flag->argument.empty()) {
            if (index + 1 >= argc) {
                return std::format("{} requires {}", argument, definition_flag->argument);
            }
            ++index;
        }
    }
    return {};
}

} // namespace

auto validate_flags(const command& definition, int argc, char** argv) -> std::string {
    if (definition.arguments == argument_policy::child_command) {
        return validate_range(definition, argc, argv, true);
    }
    if (definition.arguments != argument_policy::build_command || argc == 0) {
        return validate_range(definition, argc, argv, false);
    }
    const auto first = std::string_view{argv[0]};
    if (first == "--events") {
        return {};
    }
    if (first == "--profile") {
        return validate_range(definition, argc - 1, argv + 1, true);
    }
    return validate_range(definition, argc, argv, false);
}

} // namespace liquid_cli
