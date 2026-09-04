#pragma once

#include <filesystem>

#include "liquid/build.hpp"

namespace liquid {
class repository;
namespace objects {
struct action_record;
}
} // namespace liquid

namespace liquid::build {

[[nodiscard]] objects::sandbox_record wrap_sandbox_definition();
[[nodiscard]] liquid::result<std::filesystem::path>
wrap_cache_directory(const std::filesystem::path& working_tree);
[[nodiscard]] runtime::task<action_result> run_wrapped_action(repository& source,
                                                              const objects::action_record& action,
                                                              const action_result& expected,
                                                              std::chrono::sys_seconds observed_at);

} // namespace liquid::build
