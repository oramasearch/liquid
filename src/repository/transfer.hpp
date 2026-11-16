// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

#include "liquid/error.hpp"

namespace liquid::repository_detail {

// The key prefix of a shared-store repository's state, `repositories/<hex BLAKE3 of the
// identifier>/`, the location `repository::open` uses (liquid #326). The whole-repository transfer
// (liquid #328) reads and writes the same keys.
[[nodiscard]] result<std::string> shared_state_prefix(const std::filesystem::path& identifier);

} // namespace liquid::repository_detail
