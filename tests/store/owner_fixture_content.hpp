#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace liquid::test {

inline std::uint64_t owner_fixture_mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

// The bytes the owner fixture writes for (seed, index): between 4 KiB and about 260 KiB, so an
// object spans one or several chunks and the writers append to packs continuously.
inline std::vector<std::byte> owner_fixture_bytes(std::uint64_t seed, std::uint64_t index) {
    auto state = owner_fixture_mix((seed << 32U) ^ index);
    const auto size = 4096U + static_cast<std::size_t>(state % (256U * 1024U));
    std::vector<std::byte> bytes(size);
    for (std::size_t offset = 0; offset < size; offset += 8) {
        state = owner_fixture_mix(state);
        for (std::size_t byte = 0; byte != 8 && offset + byte < size; ++byte) {
            bytes[offset + byte] = std::byte{static_cast<unsigned char>(state >> (8U * byte))};
        }
    }
    return bytes;
}

} // namespace liquid::test
