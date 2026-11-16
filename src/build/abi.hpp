// SPDX-License-Identifier: MIT

#pragma once

#include <vector>

#include "liquid/repository.hpp"

namespace liquid::build_detail {

[[nodiscard]] auto store_abis(repository& source, action_result& value) -> runtime::task<void>;
[[nodiscard]] LIQUID_EXPORT auto abi_breaks(repository& source, const action_result& value)
    -> runtime::task<std::vector<wrap_report::abi_break>>;

} // namespace liquid::build_detail
