#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::sandbox_detail {

struct launch_request {
    std::filesystem::path sandbox_root;
    std::filesystem::path input_mount;
    std::filesystem::path toolchain;
    std::filesystem::path output_root;
    std::filesystem::path stdout_path;
    std::filesystem::path stderr_path;
    std::string work_root;
    std::string toolchain_root;
    std::string home_root;
    std::string temporary_root;
    std::string cwd;
    std::vector<std::string> command;
    std::map<std::string, std::string> environment;
    std::vector<std::string> outputs;
    std::optional<std::chrono::seconds> timeout;
};

struct launch_result {
    int exit{};
    bool timed_out{};
};

[[nodiscard]] auto launch(const launch_request& request) -> result<launch_result>;

} // namespace liquid::sandbox_detail
