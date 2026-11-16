// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "liquid/hash.hpp"

namespace liquid::vfs_detail {

enum class observed_operation {
    getattr,
    readdir,
    open,
    read,
    write,
};

struct observed_request {
    observed_operation operation;
    std::string path;
    std::int64_t pid{};
    std::optional<digest> content;
    bool denied{};
};

using observation_sink = std::function<void(observed_request)>;

} // namespace liquid::vfs_detail
