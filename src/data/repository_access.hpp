#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "liquid/data.hpp"
#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct stored_data_branch {
    data_branch_record record;
    std::string base_url;
    std::string base_fingerprint;
};

struct data_access {
    [[nodiscard]] static auto configuration(repository& source) -> result<data_config>;
    [[nodiscard]] static auto branches(repository& source)
        -> result<std::vector<stored_data_branch>>;
    [[nodiscard]] static auto root_path(repository& source) -> result<std::filesystem::path>;
    [[nodiscard]] static auto add(repository& source, stored_data_branch branch)
        -> runtime::task<void>;
    [[nodiscard]] static auto remove(repository& source, std::string id) -> runtime::task<void>;
};

} // namespace liquid::repository_detail
