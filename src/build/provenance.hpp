// SPDX-License-Identifier: MIT

#pragma once

#include "liquid/build.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {

[[nodiscard]] auto store_result_with_provenance(repository& source, action_result& value)
    -> runtime::task<result_id>;
[[nodiscard]] auto import_signed_result(repository& source, const action_result& value)
    -> runtime::task<result_id>;

namespace testing {

[[nodiscard]] LIQUID_EXPORT auto store_result_with_provenance(repository& source,
                                                              objects::action_record action,
                                                              action_result& value)
    -> runtime::task<result_id>;

} // namespace testing

} // namespace liquid::build_detail
