// SPDX-License-Identifier: MIT

#pragma once

#include "liquid/edit.hpp"

namespace liquid::edit_detail {

[[nodiscard]] auto preview_id(const edit_preview& value) -> result<digest>;
[[nodiscard]] auto apply_changes(const file_edit& edit, std::span<const std::byte> before)
    -> result<std::vector<std::byte>>;

} // namespace liquid::edit_detail
