// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <string_view>

// A reference name a repository stores and a Git export writes as a path (L-38, liquid#424 and
// liquid#387). It follows `git check-ref-format`: components separated by single `/`, none empty,
// none starting with `.` or ending with `.lock`, no `..`, no `@{`, not `@` alone, and none of
// space, control characters, `~ ^ : ? * [ \`. A name that passes can never leave the refs
// directory it is written under.
namespace liquid::repository_detail {

inline auto valid_ref_name(std::string_view name) -> bool {
    if (name.empty() || name.size() > 1024 || name == "@" || name.front() == '/' ||
        name.back() == '/' || name.back() == '.' || name.find("..") != std::string_view::npos ||
        name.find("//") != std::string_view::npos || name.find("@{") != std::string_view::npos) {
        return false;
    }
    const auto forbidden = std::string_view{" ~^:?*[\\"};
    if (std::ranges::any_of(name, [&](unsigned char c) {
            return c < 0x20 || c == 0x7f || forbidden.find(static_cast<char>(c)) != std::string_view::npos;
        })) {
        return false;
    }
    for (std::size_t start = 0; start <= name.size();) {
        const auto end = std::min(name.find('/', start), name.size());
        const auto component = name.substr(start, end - start);
        if (component.empty() || component.front() == '.' || component.ends_with(".lock")) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace liquid::repository_detail
