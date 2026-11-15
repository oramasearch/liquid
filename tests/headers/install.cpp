// SPDX-License-Identifier: MIT

#include "liquid/install.hpp"

auto install_header_isolated() -> int {
    const auto intent = liquid::install_intent{};
    return static_cast<int>(intent.v);
}
