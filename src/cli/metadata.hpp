#pragma once

#include <span>
#include <string>
#include <string_view>

namespace liquid_cli {

using command_handler = auto (*)(int argc, char** argv) -> int;

struct flag {
    std::string_view name;
    std::string_view argument;
    std::string_view description;
    std::string_view default_value;
};

enum class argument_policy {
    ordinary,
    child_command,
    build_command,
};

struct command {
    std::string_view name;
    std::string_view usage;
    std::string_view summary;
    std::string_view availability;
    std::span<const flag> flags;
    std::string_view description;
    std::string_view examples;
    std::string_view exit_status;
    argument_policy arguments{argument_policy::ordinary};
    command_handler handler;
};

[[nodiscard]] auto command_table() -> std::span<const command>;
[[nodiscard]] auto validate_flags(const command& definition, int argc, char** argv) -> std::string;
auto print_command_help(const command& definition) -> void;
auto docs(int argc, char** argv) -> int;

} // namespace liquid_cli
