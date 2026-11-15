// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build_detail {

struct relocation_file {
    std::string path;
    std::vector<std::byte> contents;
};

[[nodiscard]] auto inspect_relocation(std::span<const relocation_file> files,
                                      std::optional<std::filesystem::path> install_prefix)
    -> result<relocation_report>;

[[nodiscard]] auto relocation_check(repository& source,
                                    std::span<const action_output> outputs,
                                    std::optional<std::filesystem::path> install_prefix)
    -> runtime::task<relocation_report>;

[[nodiscard]] auto relocation_failure(const relocation_report& report) -> error;

} // namespace liquid::build_detail
