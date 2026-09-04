#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT platform {
    std::string os;
    std::string arch;
};

struct LIQUID_EXPORT action_input {
    std::vector<std::string> command;
    std::string cwd;
    std::map<std::string, std::string> env;
    std::vector<std::string> outputs;
    digest inputs;
    digest toolchain;
    liquid::platform platform;
    std::optional<std::chrono::seconds> timeout;
};

struct LIQUID_EXPORT result_id {
    digest value;
};

struct LIQUID_EXPORT action_output {
    std::string path;
    std::uint32_t mode{};
    digest blob;
};

struct LIQUID_EXPORT observed_input {
    std::string path;
    digest content;
};

struct LIQUID_EXPORT action_timing {
    std::chrono::milliseconds queued{};
    std::chrono::milliseconds sandbox{};
    std::chrono::milliseconds exec{};
    std::chrono::milliseconds upload{};
};

enum class cache_status : std::uint8_t { miss, hit };

struct LIQUID_EXPORT cache_miss {
    std::string path;
    std::optional<digest> expected;
    std::optional<digest> actual;
};

struct LIQUID_EXPORT cache_report {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t bytes_served{};
    std::uint64_t bytes_stored{};
};

struct LIQUID_EXPORT action_result {
    digest action;
    int exit{};
    std::vector<action_output> outputs;
    digest stdout_output;
    digest stderr_output;
    std::vector<observed_input> manifest;
    action_timing timing;
    std::string worker;
    std::chrono::sys_seconds observed_at{};
    actor producer;
    std::vector<std::byte> sig;
    cache_status cache{cache_status::miss};
    std::optional<cache_miss> why;
};

struct LIQUID_EXPORT wrap_options {
    std::uint32_t jobs{};
    bool cache{true};
    bool explain{};
    std::optional<std::filesystem::path> cache_directory;
};

struct LIQUID_EXPORT wrap_report {
    int exit{};
    std::uint64_t actions{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t processes_started{};
    cache_status outer{cache_status::miss};
    std::vector<cache_miss> why;
};

namespace build {

[[nodiscard]] LIQUID_EXPORT runtime::task<result_id> execute(repository& source,
                                                             action_input input);
[[nodiscard]] LIQUID_EXPORT runtime::task<action_result> result(repository& source, result_id id);
[[nodiscard]] LIQUID_EXPORT runtime::task<wrap_report> wrap(std::filesystem::path working_tree,
                                                            std::vector<std::string> command,
                                                            wrap_options options = {});

} // namespace build

} // namespace liquid
