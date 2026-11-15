// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

#include "liquid/drive.hpp"

namespace liquid::vfs_detail {

inline constexpr auto state_xattr = std::string_view{"user.liquid.state"};
inline constexpr auto pin_xattr = std::string_view{"user.liquid.pin"};

[[nodiscard]] auto drive_xattr_value(drive_path_state state, std::string_view name)
    -> result<std::string>;
[[nodiscard]] auto drive_xattr_state(std::string_view name, std::string_view value)
    -> result<drive_path_state>;
[[nodiscard]] auto drive_xattr_names() -> std::string;

} // namespace liquid::vfs_detail
