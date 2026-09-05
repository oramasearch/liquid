#pragma once

#include <filesystem>
#include <set>
#include <span>
#include <vector>

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build_detail {

[[nodiscard]] auto store_output_tree(repository& source, std::span<const action_output> outputs)
    -> runtime::task<digest>;
[[nodiscard]] auto fold_dependency_inputs(repository& source,
                                          digest base,
                                          std::span<const named_input> inputs)
    -> runtime::task<digest>;
[[nodiscard]] auto import_wrap_inputs(repository& destination,
                                      const std::filesystem::path& cache_directory,
                                      std::span<const named_input> inputs) -> runtime::task<void>;
[[nodiscard]] auto normalise_repository_outputs(repository& source,
                                                const std::set<std::string>& paths)
    -> runtime::task<void>;

} // namespace liquid::build_detail
