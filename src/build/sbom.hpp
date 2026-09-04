#pragma once

#include "liquid/build.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {

[[nodiscard]] auto store_derived_sboms(repository& source,
                                       const objects::action_record& action,
                                       action_result& value) -> runtime::task<void>;

[[nodiscard]] auto validate_derived_sboms(repository& source,
                                          result_id id,
                                          const action_result& value) -> runtime::task<bool>;

} // namespace liquid::build_detail
