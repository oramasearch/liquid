#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::tar {

struct entry {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint32_t mode{0644};
    bool directory{};
};

[[nodiscard]] auto write(std::span<const entry> entries, std::uint64_t source_date_epoch)
    -> result<std::vector<std::byte>>;

} // namespace liquid::formats::tar
