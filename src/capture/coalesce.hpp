// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <vector>

#include "capture/watcher.hpp"

namespace liquid::capture_detail {

class coalescer {
public:
    void push(watch_event event);
    [[nodiscard]] auto take() -> std::vector<watch_event>;

private:
    std::vector<watch_event> events_;
};

[[nodiscard]] auto is_temporary_path(const std::filesystem::path& path) -> bool;

} // namespace liquid::capture_detail
