#pragma once

#include "build/cache.hpp"

namespace liquid::build_detail {

[[nodiscard]] runtime::task<void>
record_trace(repository& source, disk_cache& cache, wrap_report& report);

} // namespace liquid::build_detail
