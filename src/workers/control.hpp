// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string>

#include "liquid/runtime.hpp"

namespace liquid::workers {

struct control_status {
    std::string component;
    std::function<bool()> ready;
    std::function<std::string()> metrics;
};

[[nodiscard]] auto serve_control(std::string listen, control_status status) -> runtime::task<void>;

} // namespace liquid::workers
