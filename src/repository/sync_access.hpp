// SPDX-License-Identifier: MIT

#pragma once

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct sync_access {
    [[nodiscard]] LIQUID_EXPORT static auto objects(repository& source) -> store&;
};

} // namespace liquid::repository_detail
