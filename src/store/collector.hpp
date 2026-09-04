#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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
load_collection_manifest(std::filesystem::path root);
[[nodiscard]] runtime::task<void> save_collection_manifest(const std::filesystem::path& root,
                                                           const collection_manifest& manifest);

class collector {
public:
    explicit collector(store_backend& store) : store_(store) {}
    [[nodiscard]] runtime::task<collection_report> run(collection_options options);

private:
    store_backend& store_;
};

} // namespace liquid::store_detail
