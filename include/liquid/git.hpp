#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT import_report {
    std::uint64_t commits{};
    std::uint64_t blobs{};
    std::uint64_t skipped_commits{};
    std::uint64_t peak_anonymous_bytes{};
    std::vector<std::string> refs;
};

struct LIQUID_EXPORT export_report {
    std::uint64_t commits{};
    std::vector<std::string> degradations;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<import_report> import_git(repository& destination,
                                                                    std::filesystem::path source);
[[nodiscard]] LIQUID_EXPORT runtime::task<export_report>
export_git(repository& source, std::filesystem::path target, std::string ref);

} // namespace liquid
