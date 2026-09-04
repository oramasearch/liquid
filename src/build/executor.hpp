#pragma once

#include <chrono>

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build {

[[nodiscard]] runtime::task<action_result> run_action(repository& source,
                                                      action_input input,
                                                      digest action,
                                                      digest sandbox,
                                                      std::chrono::sys_seconds observed_at);

} // namespace liquid::build
