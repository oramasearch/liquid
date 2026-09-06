#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "liquid/vfs.hpp"
#include "vfs/repository_access.hpp"

namespace liquid::vfs_detail {

class hydration_cache {
public:
    explicit hydration_cache(std::filesystem::path root);

    [[nodiscard]] auto initialise() -> runtime::task<void>;
    [[nodiscard]] auto read(repository& source,
                            const repository_detail::vfs_entry& file,
                            std::uint64_t offset,
                            std::size_t size) -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] auto hydrate(repository& source, const repository_detail::vfs_entry& file)
        -> runtime::task<void>;
    void invalidate(node_id node);
    [[nodiscard]] auto status() const -> mount_status;

private:
    struct file_state {
        std::vector<digest> chunks;
        std::vector<std::uint64_t> chunk_sizes;
        std::vector<bool> hydrated;
    };

    [[nodiscard]] auto load_chunk(repository& source,
                                  digest content,
                                  digest id,
                                  std::uint64_t offset,
                                  std::uint64_t size) -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] auto chunk_path(const digest& id) const -> std::filesystem::path;
    void record_chunk(const repository_detail::vfs_entry& file, std::size_t index);

    static constexpr std::uint64_t capacity = 1024ULL * 1024ULL * 1024ULL;
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::map<node_id, file_state> files_;
    mount_status status_{};
};

} // namespace liquid::vfs_detail
