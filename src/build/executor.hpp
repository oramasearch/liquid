// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build {

struct prepared_action {
    action_input input;
    digest action;
    digest sandbox;
    digest candidate;
    std::chrono::sys_seconds observed_at{};
    std::optional<result_id> hit;
    std::optional<cache_miss> why;
    std::uint64_t bytes_served{};
};

[[nodiscard]] runtime::task<prepared_action>
prepare_action(repository& source, action_input input, bool lookup_cache = true);

[[nodiscard]] runtime::task<result_id> execute_with_history_candidate(repository& source,
                                                                      action_input input,
                                                                      remote_executor& remote,
                                                                      digest history_candidate);

[[nodiscard]] runtime::task<action_result> run_action(repository& source,
                                                      action_input input,
                                                      digest action,
                                                      digest sandbox,
                                                      std::chrono::sys_seconds observed_at);

[[nodiscard]] runtime::task<result_id>
finish_action(repository& source, const prepared_action& prepared, action_result value);

} // namespace liquid::build
