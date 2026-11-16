// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <map>
#include <string>

#include "liquid/build.hpp"
#include "objects/records.hpp"

namespace liquid::sandbox_detail {

struct environment_result {
    std::map<std::string, std::string> values;
    std::string work_root;
    std::string toolchain_root;
    std::string home_root;
    std::string temporary_root;
};

[[nodiscard]] auto definition(const platform& target) -> result<objects::sandbox_record>;
[[nodiscard]] auto environment(const action_input& input,
                               const digest& action,
                               std::chrono::sys_seconds source_time) -> result<environment_result>;

} // namespace liquid::sandbox_detail
