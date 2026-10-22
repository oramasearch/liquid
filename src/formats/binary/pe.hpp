#pragma once

#include "formats/binary/elf.hpp"

namespace liquid::formats::binary {

[[nodiscard]] auto inspect_pe(std::span<const std::byte> bytes) -> result<image>;
[[nodiscard]] auto inspect_pe(std::span<const std::byte> bytes, std::uint64_t byte_budget)
    -> result<image>;

} // namespace liquid::formats::binary
