// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "liquid/object.hpp"
#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct vfs_entry {
    std::string name;
    node_kind kind{node_kind::blob};
    std::uint32_t mode{};
    node_id node{};
    digest object{};
    std::uint64_t size{};
    std::vector<digest> chunks;
    std::vector<std::uint64_t> chunk_sizes;
    digest content{};
    std::optional<remote_blob_location> remote;
    bool remotely_available{};
};

struct LIQUID_EXPORT vfs_access {
    [[nodiscard]] static auto entry(repository& source, std::string path)
        -> runtime::task<vfs_entry>;
    [[nodiscard]] static auto entry(repository& source, digest root, std::string path)
        -> runtime::task<vfs_entry>;
    [[nodiscard]] static auto children(repository& source, std::string path)
        -> runtime::task<std::vector<vfs_entry>>;
    [[nodiscard]] static auto children(repository& source, digest root, std::string path)
        -> runtime::task<std::vector<vfs_entry>>;
    [[nodiscard]] static auto
    read_chunk(repository& source, digest id, std::uint64_t offset, std::span<std::byte> into)
        -> runtime::task<std::uint64_t>;
    [[nodiscard]] static auto
    make_directory(repository& source, std::string path, std::uint32_t mode) -> runtime::task<void>;
    [[nodiscard]] static auto set_mode(repository& source, std::string path, std::uint32_t mode)
        -> runtime::task<void>;
    [[nodiscard]] static auto root_path(repository& source) -> result<std::filesystem::path>;
    [[nodiscard]] static auto register_remote_source(repository& source,
                                                     std::string id,
                                                     std::shared_ptr<remote_blob_source> remote)
        -> runtime::task<void>;
};

} // namespace liquid::repository_detail
