// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

#include "liquid/build.hpp"

namespace liquid {
class repository;
}

namespace liquid::build_detail {

[[nodiscard]] runtime::task<void> store_test(repository& source, test_target target);
[[nodiscard]] runtime::task<std::vector<test_target>> load_tests(repository& source);
[[nodiscard]] runtime::task<void>
discover_cmake_tests(repository& source, std::filesystem::path working_tree, digest toolchain);
[[nodiscard]] result<build_handle> start_tests(repository& source, std::string filter);

} // namespace liquid::build_detail
