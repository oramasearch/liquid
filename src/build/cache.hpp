#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {

struct cache_lookup {
    std::optional<result_id> hit;
    std::optional<cache_miss> why;
    std::uint64_t bytes_served{};
    std::uint64_t bytes_avoided{};
};

class LIQUID_EXPORT action_index {
public:
    explicit action_index(std::filesystem::path directory);

    [[nodiscard]] runtime::task<void> open();
    [[nodiscard]] std::vector<digest> candidates(digest candidate) const;
    [[nodiscard]] result<void> insert(digest candidate, digest result_id);
    [[nodiscard]] runtime::task<void> rebuild(std::vector<objects::action_index_entry> entries,
                                              digest root);
    [[nodiscard]] runtime::task<void> persist(digest root);

    [[nodiscard]] const std::vector<objects::action_index_entry>& entries() const noexcept;
    [[nodiscard]] digest root() const noexcept;

private:
    std::filesystem::path directory_;
    std::vector<objects::action_index_entry> entries_;
    digest root_{};
    std::uint64_t next_sequence_{1};
};

class LIQUID_EXPORT disk_cache {
public:
    explicit disk_cache(repository& source);

    [[nodiscard]] runtime::task<void> open();
    [[nodiscard]] result<digest> candidate_key(const action_input& input, digest sandbox) const;
    [[nodiscard]] runtime::task<cache_lookup> lookup(const action_input& input, digest sandbox);
    [[nodiscard]] runtime::task<std::optional<result_id>>
    find_output(std::string_view path, std::optional<digest> content = std::nullopt);
    [[nodiscard]] runtime::task<void>
    publish(digest candidate, result_id id, const action_result& value);
    [[nodiscard]] runtime::task<void>
    record_verification(digest result, digest attestation, bool divergent);
    [[nodiscard]] const std::vector<objects::action_index_entry>& entries() const noexcept;
    [[nodiscard]] bool divergent(digest result) const noexcept;
    [[nodiscard]] result<void>
    record_lookup(cache_status status, result_id id, std::optional<result_id> previous = {});

private:
    repository& source_;
    action_index index_;
    std::filesystem::path directory_;

    [[nodiscard]] runtime::task<std::optional<digest>> load_root_pointer();
    [[nodiscard]] runtime::task<void> save_root_pointer(digest root);
    [[nodiscard]] runtime::task<void> persist_root();
};

namespace testing {

LIQUID_EXPORT void reset_sandbox_spawn_count() noexcept;
[[nodiscard]] LIQUID_EXPORT std::uint64_t sandbox_spawn_count() noexcept;
LIQUID_EXPORT void record_sandbox_spawn() noexcept;
[[nodiscard]] LIQUID_EXPORT runtime::task<void> release_cache_root(repository& source);

} // namespace testing

} // namespace liquid::build_detail
