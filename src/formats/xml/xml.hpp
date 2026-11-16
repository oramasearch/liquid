// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

#include "liquid/error.hpp"

namespace liquid::formats::xml {

[[nodiscard]] auto canonicalise(std::string_view input, bool omit_relationship_ids = false)
    -> result<std::string>;

} // namespace liquid::formats::xml
