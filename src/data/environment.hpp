#pragma once

#include <map>
#include <string>

#include "liquid/data.hpp"

namespace liquid {
class repository;
}

namespace liquid::data_detail {

[[nodiscard]] LIQUID_EXPORT auto session_environment(repository& source, session_id session)
    -> result<std::map<std::string, std::string>>;

} // namespace liquid::data_detail
