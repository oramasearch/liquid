#pragma once

#include <filesystem>

#include "liquid/repository.hpp"

namespace liquid::build_detail {

[[nodiscard]] auto materialise_tree(repository& source,
                                    digest root,
                                    std::filesystem::path destination) -> runtime::task<void>;

} // namespace liquid::build_detail
