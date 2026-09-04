#pragma once

#include <vector>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct git_access {
    [[nodiscard]] static runtime::task<digest> put_blob(repository& destination,
                                                        std::vector<std::byte> contents);
    [[nodiscard]] static runtime::task<digest> put_tree(repository& destination, tree value);
};

} // namespace liquid::repository_detail
