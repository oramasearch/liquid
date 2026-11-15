// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>

#include "liquid/build.hpp"
#include "liquid/repository.hpp"

namespace liquid::build_detail {

struct lookup_record {
    cache_status status{cache_status::miss};
    result_id result;
    std::optional<result_id> previous;
};

[[nodiscard]] LIQUID_EXPORT auto
wrap_cache_directory(const std::filesystem::path& working_tree,
                     const std::optional<std::filesystem::path>& requested = {})
    -> result<std::filesystem::path>;
[[nodiscard]] auto append_lookup(const std::filesystem::path& action_directory,
                                 const lookup_record& value) -> result<void>;
[[nodiscard]] auto read_lookups(const std::filesystem::path& action_directory)
    -> result<std::vector<lookup_record>>;
[[nodiscard]] auto classify_miss(repository& source,
                                 const action_input& actual,
                                 digest actual_sandbox,
                                 std::optional<result_id> previous,
                                 const action_result* actual_result = nullptr)
    -> runtime::task<cache_miss>;
[[nodiscard]] auto output_bytes(repository& source, const action_result& value)
    -> runtime::task<std::uint64_t>;
[[nodiscard]] LIQUID_EXPORT auto update_cache_stats(const std::filesystem::path& cache_directory,
                                                    const wrap_report& report) -> result<void>;

} // namespace liquid::build_detail
