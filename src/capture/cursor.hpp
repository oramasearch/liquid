// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>

#include "crdt/journal.hpp"

namespace liquid::capture_detail {

struct capture_cursor {
    crdt::journal::cursor journal{};
    std::uint64_t platform_token{0};
};

[[nodiscard]] auto load_cursor(const std::filesystem::path& path) -> result<capture_cursor>;
[[nodiscard]] auto save_cursor(const std::filesystem::path& path, capture_cursor value)
    -> result<void>;

} // namespace liquid::capture_detail
