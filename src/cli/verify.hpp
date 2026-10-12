#pragma once

#include <filesystem>
#include <functional>

#include "liquid/build.hpp"
#include "liquid/error.hpp"

namespace liquid_cli::verify_detail {

using verification_runner = std::function<liquid::result<liquid::verify_report>(
    std::filesystem::path, liquid::verify_options)>;

auto run(int argc, char** argv, const verification_runner& runner) -> int;

} // namespace liquid_cli::verify_detail
