// SPDX-License-Identifier: MIT

#pragma once
#include <string_view>

#include "liquid/export.hpp"

namespace liquid {

// Semantic version of this build.
struct LIQUID_EXPORT version {
    unsigned major;
    unsigned minor;
    unsigned patch;
};

// constexpr functions are implicitly inline and must be visible to every
// translation unit that uses them, so this lives in the header.
[[nodiscard]] LIQUID_EXPORT constexpr version current_version() noexcept {
    return {0, 3, 0};
}

[[nodiscard]] LIQUID_EXPORT std::string_view version_string() noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view grammar_digest() noexcept;

} // namespace liquid
