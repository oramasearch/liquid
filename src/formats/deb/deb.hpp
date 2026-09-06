#pragma once

#include <span>
#include <string>
#include <vector>

#include "formats/package/common.hpp"
#include "liquid/projection.hpp"

namespace liquid::formats::deb {

struct archive {
    std::string control;
    std::vector<std::string> files;
};

[[nodiscard]] auto write(const package_metadata& metadata,
                         const projection_options& options,
                         std::span<const std::string> dependencies,
                         std::span<const package::file> files) -> result<std::vector<std::byte>>;
[[nodiscard]] auto read(std::span<const std::byte> bytes,
                        std::uint64_t maximum_output = 512ULL * 1024ULL * 1024ULL)
    -> result<archive>;

} // namespace liquid::formats::deb
