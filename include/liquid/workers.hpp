// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "liquid/build.hpp"
#include "liquid/export.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT scheduler_options {
    std::filesystem::path store_directory;
    std::string listen{"127.0.0.1:50051"};
    std::string control_listen{"127.0.0.1:8080"};
    std::string instance_name;
    std::chrono::milliseconds lease_duration{std::chrono::seconds{2}};
};

struct LIQUID_EXPORT worker_options {
    remote_config scheduler;
    std::filesystem::path root_directory;
    std::string control_listen{"127.0.0.1:8081"};
    action_size_class size_class{action_size_class::long_action};
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{100}};
};

[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_scheduler(scheduler_options options);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> run_worker(worker_options options);

} // namespace liquid
