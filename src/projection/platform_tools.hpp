// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "liquid/hash.hpp"

namespace liquid::projection_detail {

struct tool_result {
    int exit{};
    std::string output;
    std::string error_output;
};

[[nodiscard]] auto run_tool(const std::vector<std::string>& command,
                            const std::filesystem::path& working_directory = {},
                            const std::map<std::string, std::string>& environment = {})
    -> result<tool_result>;
[[nodiscard]] auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>>;
[[nodiscard]] auto read_text_file(const std::filesystem::path& path) -> result<std::string>;
[[nodiscard]] auto write_text_file(const std::filesystem::path& path, std::string_view value)
    -> result<void>;
[[nodiscard]] auto sha256_file(const std::filesystem::path& path) -> result<digest>;
[[nodiscard]] auto temporary_path(std::string_view label) -> std::filesystem::path;
[[nodiscard]] auto remove_known_path(const std::filesystem::path& path) -> result<void>;

} // namespace liquid::projection_detail
