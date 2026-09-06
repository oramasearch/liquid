#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "formats/package/common.hpp"
#include "liquid/error.hpp"

namespace liquid::formats::squashfs {

[[nodiscard]] auto write(std::span<const package::file> files, std::uint64_t source_date_epoch)
    -> result<std::vector<std::byte>>;

} // namespace liquid::formats::squashfs
