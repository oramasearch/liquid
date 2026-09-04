#pragma once

#include <optional>
#include <string>

#include "liquid/object.hpp"

namespace liquid::repository_detail {

[[nodiscard]] auto entry_conflict(std::string path, const digest &left, const digest &right)
    -> std::optional<conflict>;

} // namespace liquid::repository_detail
