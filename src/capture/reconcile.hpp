#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <vector>

#include "capture/watcher.hpp"

namespace liquid::capture_detail {

struct file_state {
    std::uintmax_t size{0};
    std::filesystem::file_time_type modified{};
};

using workspace_state = std::map<std::filesystem::path, file_state>;

struct reconcile_result {
    workspace_state state;
    std::vector<watch_event> changes;
};

[[nodiscard]] auto load_workspace_state(const std::filesystem::path& path)
    -> result<workspace_state>;
[[nodiscard]] auto save_workspace_state(const std::filesystem::path& path,
                                        const workspace_state& state) -> result<void>;
[[nodiscard]] auto reconcile(const std::filesystem::path& workspace,
                             const workspace_state& previous,
                             const std::function<bool(const std::filesystem::path&)>& excluded)
    -> result<reconcile_result>;

} // namespace liquid::capture_detail
