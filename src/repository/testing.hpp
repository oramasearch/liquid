#pragma once

#include <cstdint>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct LIQUID_EXPORT testing {
  static auto directory_loads(repository &value) -> std::uint64_t;
  static auto last_operation(repository &value) -> op_id;
  static void reset_directory_loads(repository &value);
};

} // namespace liquid::repository_detail
