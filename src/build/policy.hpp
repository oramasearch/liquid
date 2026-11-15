// SPDX-License-Identifier: MIT

#pragma once

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build_detail {

[[nodiscard]] result<void> validate_policy(const policy_config& policy);
[[nodiscard]] result<digest> policy_digest(const policy_config& policy);
[[nodiscard]] runtime::task<policy_report> enforce_policy(repository& source,
                                                          const action_input& input);
[[nodiscard]] error policy_refusal(const policy_report& report);

} // namespace liquid::build_detail
