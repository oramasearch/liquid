// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "liquid/export.hpp"

namespace liquid {

// The Lake bearer this process uses. It comes from LIQUID_TOKEN_FILE, a file only its owner may
// read, or else from LIQUID_TOKEN; it is read once and empty when neither is set. A file readable
// by its group or others is refused, with a message on standard error.
[[nodiscard]] LIQUID_EXPORT std::string lake_token();

// For a long-lived process (the daemon): reads the bearer, then wipes LIQUID_TOKEN from this
// process's memory and environment, so /proc/<pid>/environ and the processes it starts no longer
// carry it. `lake_token()` keeps returning the value read.
LIQUID_EXPORT void scrub_environment_token();

} // namespace liquid
