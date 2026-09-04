#pragma once

#include <cstdint>
#include <filesystem>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct LIQUID_EXPORT testing {
  static auto directory_loads(repository &value) -> std::uint64_t;
  static auto last_operation(repository &value) -> op_id;
  static auto clone(const repository &source, std::filesystem::path destination)
      -> runtime::task<repository>;
  static auto sync(repository &left, repository &right) -> runtime::task<void>;
  static void reset_directory_loads(repository &value);
};

} // namespace liquid::repository_detail
