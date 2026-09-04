#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace liquid::git_detail {

using sha1 = std::array<std::byte, 20>;

[[nodiscard]] auto sha1_hash(std::span<const std::byte> input) -> sha1;
[[nodiscard]] auto sha1_hex(const sha1& value) -> std::string;
[[nodiscard]] auto parse_sha1(std::string_view text) -> std::optional<sha1>;

} // namespace liquid::git_detail
