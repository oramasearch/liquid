#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "liquid/build.hpp"

namespace liquid {
class repository;
namespace objects {
struct action_record;
struct sandbox_record;
} // namespace objects
} // namespace liquid

namespace liquid::build {

[[nodiscard]] objects::sandbox_record wrap_sandbox_definition(bool network = false);
[[nodiscard]] liquid::result<std::filesystem::path>
wrap_cache_directory(const std::filesystem::path& working_tree);
[[nodiscard]] runtime::task<std::pair<digest, std::string>>
phase2_toolchain(repository& source,
                 digest recorded,
                 digest inputs,
                 std::string_view cwd,
                 std::string_view command);
[[nodiscard]] runtime::task<digest> capture_toolchain(repository& source,
                                                      std::set<std::string> executables);
[[nodiscard]] runtime::task<action_result> run_wrapped_action(repository& source,
                                                              const objects::action_record& action,
                                                              const action_result& expected,
                                                              std::chrono::sys_seconds observed_at);

} // namespace liquid::build
