#include <cstddef>
#include <span>

#include "liquid/hot_abi.h"
#include "liquid/identity.hpp"

auto main() -> int {
    const auto decoded = liquid::decode_identity_document(std::span<const std::byte>{});
    return !decoded && liquid_hot_abi_version() == LIQUID_HOT_ABI_VERSION ? 0 : 1;
}
