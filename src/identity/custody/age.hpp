#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::custody_detail {

[[nodiscard]] auto age_decrypt(std::span<const std::byte> input,
                               std::span<const std::string_view> identities)
    -> result<std::vector<std::byte>>;

} // namespace liquid::custody_detail
