// SPDX-License-Identifier: MIT

#pragma once

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build_detail {

[[nodiscard]] result<build_handle> start_repository(repository& source, build_target target);

} // namespace liquid::build_detail
