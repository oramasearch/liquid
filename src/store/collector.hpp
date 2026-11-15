// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid::store_detail {

class store_backend;

struct collection_manifest {
    std::uint64_t epoch{};
    std::map<std::uint32_t, std::vector<std::byte>> tombstones;

    [[nodiscard]] bool dead(std::uint32_t pack_id, std::size_t frame) const noexcept;
};

[[nodiscard]] runtime::task<collection_manifest>
load_collection_manifest(std::shared_ptr<::liquid::store_backend> backend,
                         std::filesystem::path context);
[[nodiscard]] runtime::task<void>
save_collection_manifest(std::shared_ptr<::liquid::store_backend> backend,
                         const collection_manifest& manifest);

class collector {
public:
    explicit collector(store_backend& store) : store_(store) {}
    [[nodiscard]] runtime::task<collection_report> run(collection_options options);
    [[nodiscard]] runtime::task<void> redact(std::span<const digest> ids);
    [[nodiscard]] runtime::task<void> evict(digest id);

private:
    [[nodiscard]] runtime::task<void> remove_local(collection_manifest manifest,
                                                   std::set<std::string> target_objects,
                                                   std::set<std::string> target_chunks,
                                                   std::optional<std::string> excluded_remote,
                                                   bool compact,
                                                   bool refresh_subjects);

    store_backend& store_;
};

} // namespace liquid::store_detail
