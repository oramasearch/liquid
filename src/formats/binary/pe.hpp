// SPDX-License-Identifier: MIT

#pragma once

#include "formats/binary/elf.hpp"

namespace liquid::formats::binary {

[[nodiscard]] auto inspect_pe(std::span<const std::byte> bytes) -> result<image>;

} // namespace liquid::formats::binary
