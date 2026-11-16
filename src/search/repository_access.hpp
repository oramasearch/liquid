// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

#include "liquid/error.hpp"
#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct search_access {
    [[nodiscard]] static auto index_path(repository& source) -> result<std::filesystem::path>;
};

} // namespace liquid::repository_detail
