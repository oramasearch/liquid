#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
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
    bool remote_eligible{};
    std::stop_token stop;
};

struct launch_result {
    int exit{};
    bool timed_out{};
    std::optional<std::string> forbidden_read;
    bool cancelled{};
};

struct wrap_launch_request {
    std::filesystem::path sandbox_root;
    std::filesystem::path input_mount;
    std::filesystem::path toolchain_mount;
    std::filesystem::path hook;
    std::filesystem::path trace;
    std::filesystem::path plan;
    std::filesystem::path stdout_path;
    std::filesystem::path stderr_path;
    std::string cwd{"/liquid/work"};
    std::vector<std::string> command;
    std::map<std::string, std::string> environment;
    bool network{};
    bool network_only{};
    std::stop_token stop;
};

[[nodiscard]] auto launch(const launch_request& request) -> result<launch_result>;
[[nodiscard]] auto launch_wrap(const wrap_launch_request& request) -> result<launch_result>;
#if defined(__linux__)
[[nodiscard]] auto wrap_namespace_arguments(bool network) -> std::vector<std::string>;
#endif

} // namespace liquid::sandbox_detail
