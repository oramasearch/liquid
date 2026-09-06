#pragma once

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/drive.hpp"
#include "liquid/repository.hpp"

namespace liquid::drive_detail {

inline constexpr auto settings_metadata_key = "drive.settings.v1";

struct stored_settings {
    drive_settings policy;
    std::map<std::string, drive_path_state, std::less<>> states;
    std::set<std::string, std::less<>> hydrated;
    std::string offline_reason;
};

[[nodiscard]] auto normalize_path(std::string path) -> result<std::string>;
[[nodiscard]] auto state_name(drive_path_state state) -> std::string_view;
[[nodiscard]] auto parse_state(std::string_view value) -> result<drive_path_state>;
[[nodiscard]] auto effective_state(const stored_settings& settings, std::string_view path)
    -> drive_path_state;
[[nodiscard]] auto should_hydrate_fully(const stored_settings& settings,
                                        std::string_view path,
                                        std::uint64_t size) -> bool;
[[nodiscard]] auto load_settings(repository& source) -> runtime::task<stored_settings>;
[[nodiscard]] auto save_settings(repository& source, const stored_settings& settings)
    -> runtime::task<void>;
[[nodiscard]] auto set_path_state(repository& source, std::string path, drive_path_state state)
    -> runtime::task<stored_settings>;
[[nodiscard]] auto record_hydrated(repository& source, std::string path) -> runtime::task<void>;
[[nodiscard]] auto record_offline_reason(repository& source, std::string reason)
    -> runtime::task<void>;

} // namespace liquid::drive_detail
