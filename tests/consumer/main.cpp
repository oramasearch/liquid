#include <cstddef>
#include <span>

#include "liquid/identity.hpp"

auto main() -> int {
    const auto decoded = liquid::decode_identity_document(std::span<const std::byte>{});
    return !decoded ? 0 : 1;
}
