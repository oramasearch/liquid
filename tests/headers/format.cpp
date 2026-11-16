// SPDX-License-Identifier: MIT

#include "liquid/format.hpp"

auto format_header_isolated() -> int {
    return static_cast<int>(liquid::format_registry::global().codecs().size());
}
