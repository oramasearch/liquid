// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT reapi_serve_options {
    std::filesystem::path store_directory;
    std::string listen{"127.0.0.1:50051"};
    std::string instance_name;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_reapi(reapi_serve_options options);

} // namespace liquid
