// SPDX-License-Identifier: MIT

#include "liquid/credential.hpp"

#include <string>

#include "credential_token.hpp"

namespace liquid {

std::string lake_token() {
    return token_detail::lake_token();
}

void scrub_environment_token() {
    token_detail::scrub_environment_token();
}

} // namespace liquid
