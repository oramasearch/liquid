// SPDX-License-Identifier: MIT

#include "xattrs.hpp"

#include "drive/settings.hpp"

namespace liquid::vfs_detail {

auto drive_xattr_value(drive_path_state state, std::string_view name) -> result<std::string> {
    if (name == state_xattr) {
        return std::string{drive_detail::state_name(state)};
    }
    if (name == pin_xattr) {
        return state == drive_path_state::available_offline ? "1" : "0";
    }
    return std::unexpected(
        error{errc::not_found, "unknown drive extended attribute", std::string{name}});
}

auto drive_xattr_state(std::string_view name, std::string_view value) -> result<drive_path_state> {
    if (name == state_xattr) {
        return drive_detail::parse_state(value);
    }
    if (name == pin_xattr) {
        if (value == "1") {
            return drive_path_state::available_offline;
        }
        if (value == "0") {
            return drive_path_state::online_only;
        }
        return std::unexpected(error{errc::invalid_argument,
                                     "drive pin extended attribute must be 0 or 1",
                                     std::string{value}});
    }
    return std::unexpected(
        error{errc::not_found, "unknown drive extended attribute", std::string{name}});
}

auto drive_xattr_names() -> std::string {
    auto output = std::string{state_xattr};
    output.push_back('\0');
    output.append(pin_xattr);
    output.push_back('\0');
    return output;
}

} // namespace liquid::vfs_detail
