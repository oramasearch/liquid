#pragma once

#include <string>

#include "liquid/lake.hpp"
#include "liquid/repository.hpp"

namespace liquid_cli {

// Resolves what a sync, clone or review command talks to (`argument`: empty, a lake name or a
// full URL), checks the lake's version once, and prints a warning when the version is unknown.
// An unsupported lake is an `unsupported` error naming the lake, its version and liquid's range.
[[nodiscard]] auto prepare_lake(liquid::repository* subject, std::string argument)
    -> liquid::result<liquid::lake_target>;

// Records a named lake in a repository that has none yet, after it synced with that lake.
auto remember_lake(liquid::repository& subject, const liquid::lake_target& target) -> void;

} // namespace liquid_cli
