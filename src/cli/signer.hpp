// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

#include "liquid/identity.hpp"

namespace liquid_cli {

// The author a command prints for a record. A record's author is shown only when an identity
// chain verified it; otherwise it is `liquid::unverified_signer_label`, never the `taap_id` the
// record claims (liquid#429, liquid#508). Every command that prints an author, in text or JSON,
// uses this, and JSON output also carries `"identity_verified": true|false`.
[[nodiscard]] inline auto shown_signer(const liquid::actor& by, bool identity_verified)
    -> std::string_view {
    return identity_verified ? std::string_view{by.taap_id} : liquid::unverified_signer_label;
}

} // namespace liquid_cli
