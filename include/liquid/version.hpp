#pragma once
#include <string_view>

namespace liquid {

// Semantic version of this build.
struct version {
    unsigned major;
    unsigned minor;
    unsigned patch;
};

// constexpr functions are implicitly inline and must be visible to every
// translation unit that uses them, so this lives in the header.
[[nodiscard]] constexpr version current_version() noexcept { return {0, 0, 1}; }

[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace liquid
