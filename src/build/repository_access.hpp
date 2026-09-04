#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <vector>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct build_access {
    [[nodiscard]] static auto get(repository& source, digest id)
        -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] static auto put_record(repository& source, std::vector<std::byte> bytes)
        -> runtime::task<digest>;
    [[nodiscard]] static auto put_file(repository& source, std::filesystem::path path)
        -> runtime::task<digest>;
    [[nodiscard]] static auto
    read(repository& source, digest id, std::uint64_t offset, std::span<std::byte> output)
        -> runtime::task<std::uint64_t>;
    [[nodiscard]] static auto change_time(repository& source, digest tree)
        -> runtime::task<std::chrono::sys_seconds>;
    [[nodiscard]] static auto root_path(repository& source) -> result<std::filesystem::path>;
};

} // namespace liquid::repository_detail
