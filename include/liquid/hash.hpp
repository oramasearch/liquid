#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid {

enum class algorithm : std::uint8_t { blake3 = 1, sha256 = 2 };

struct LIQUID_EXPORT digest {
    algorithm alg;
    std::array<std::byte, 32> bytes;
};

[[nodiscard]] LIQUID_EXPORT result<digest> hash(
    algorithm alg, std::span<const std::byte> input);
[[nodiscard]] LIQUID_EXPORT std::string to_string(const digest& value);
[[nodiscard]] LIQUID_EXPORT result<digest> parse_digest(std::string_view text);

}  // namespace liquid
