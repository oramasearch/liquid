#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "formats/package/common.hpp"
#include "liquid/format.hpp"
#include "liquid/projection.hpp"

namespace liquid::formats::apk {

struct archive {
    std::string pkginfo;
    std::vector<std::string> files;
    std::vector<package::file> payload;
    std::optional<generated_maintainer_scripts> scripts;
    std::vector<format_entry> control_entries;
    std::vector<format_entry> data_entries;
};

[[nodiscard]] auto write(const package_metadata& metadata,
                         const projection_options& options,
                         std::span<const std::string> dependencies,
                         std::span<const package::file> files,
                         const generated_maintainer_scripts* scripts = nullptr)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto read(std::span<const std::byte> bytes,
                        std::uint64_t maximum_output = 512ULL * 1024ULL * 1024ULL)
    -> result<archive>;

} // namespace liquid::formats::apk
