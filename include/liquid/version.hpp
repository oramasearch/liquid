#pragma once
#include <string_view>

namespace liquid {

// Semantic version of this build. The single source is CMake's project(VERSION).
struct version {
    unsigned major;
    unsigned minor;
    unsigned patch;
};

[[nodiscard]] constexpr version current_version() noexcept;
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace liquid
