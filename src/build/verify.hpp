#pragma once

#include <filesystem>

namespace liquid::build_detail {

void schedule_graph_validation(std::filesystem::path working_tree,
                               std::filesystem::path cache_directory);

} // namespace liquid::build_detail
