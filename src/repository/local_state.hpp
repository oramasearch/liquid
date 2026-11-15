#pragma once

#include <array>
#include <string_view>

namespace liquid::repository_detail {

// Top-level names a checkout keeps for its own state (liquid #420). They hold the repository
// document marker and journal, the object store, daemon, capture, release, index and replication
// state, build and wrap caches, and data-branch state. None of them is ever repository content:
// tree import never records them, and a drive never writes one from a peer's tree, so a clone or
// sync cannot replace the local repository document or any trust configuration kept beside it.
inline constexpr auto local_state_files = std::to_array<std::string_view>({
    "LIQUID_REPO",
    "LIQUID_JOURNAL",
});

inline constexpr auto local_state_directories = std::to_array<std::string_view>({
    ".liquid",
    ".liquid-store",
    ".liquid-build",
    ".liquid-data",
    ".liquid-wrap-cache",
});

inline constexpr auto capture_cookie_prefix = std::string_view{".liquid-capture-cookie-"};

// True when a repository-relative path (`/`-separated, no leading slash) names checkout-local
// state rather than repository content.
inline auto local_state_path(std::string_view path) -> bool {
    for (const auto name : local_state_files) {
        if (path == name) {
            return true;
        }
    }
    const auto separator = path.find('/');
    const auto first = separator == std::string_view::npos ? path : path.substr(0, separator);
    for (const auto name : local_state_directories) {
        if (first == name) {
            return true;
        }
    }
    return first.starts_with(capture_cookie_prefix);
}

} // namespace liquid::repository_detail
