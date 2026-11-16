// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/vfs.hpp"

namespace liquid::repository_detail {

struct vfs_read_result {
    std::vector<std::byte> bytes;
    mount_status status;
};

struct LIQUID_EXPORT testing {
    static auto directory_loads(repository& value) -> std::uint64_t;
    static auto last_operation(repository& value) -> op_id;
    static auto last_operation_node(repository& value) -> node_id;
    static auto cached_vfs_read(repository& value,
                                std::string path,
                                std::filesystem::path cache,
                                std::uint64_t offset,
                                std::size_t size) -> runtime::task<vfs_read_result>;
    static auto clone(const repository& source, std::filesystem::path destination)
        -> runtime::task<repository>;
    static auto sync(repository& left, repository& right) -> runtime::task<void>;
    static auto verify_history(repository& value) -> result<std::uint64_t>;
    static void reset_directory_loads(repository& value);
    // Renames a root entry of the working tree in memory only, bypassing name validation, so a
    // test can hand a materialiser a name no write or decode would accept.
    static auto rename_working_entry(repository& value, const std::string& from, std::string to)
        -> bool;
};

} // namespace liquid::repository_detail
