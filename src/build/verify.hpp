#pragma once

namespace liquid {
class repository;
}

namespace liquid::build_detail {

void schedule_verification(repository source);

} // namespace liquid::build_detail
