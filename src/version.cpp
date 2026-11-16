// SPDX-License-Identifier: MIT

#include "liquid/version.hpp"

namespace liquid {

std::string_view version_string() noexcept {
    return "0.2.0";
}

std::string_view grammar_digest() noexcept {
    return LIQUID_GRAMMAR_LOCK_DIGEST;
}

} // namespace liquid
