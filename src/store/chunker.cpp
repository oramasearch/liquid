#include "store/chunker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace liquid::store_detail {
namespace {

constexpr std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

constexpr std::array<std::uint64_t, 256> make_gear_table() noexcept {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t state = 0x6c69717569642d31ULL;
    for (auto& value : table) value = splitmix64(state);
    return table;
}

inline constexpr auto gear = make_gear_table();
inline constexpr std::uint64_t early_mask = (std::uint64_t{1} << 17U) - 1U;
inline constexpr std::uint64_t late_mask = (std::uint64_t{1} << 15U) - 1U;

std::size_t next_boundary(std::span<const std::byte> input) noexcept {
    if (input.size() <= chunk_min_size) return input.size();

    const auto limit = std::min(input.size(), chunk_max_size);
    const auto target = std::min(limit, chunk_target_size);
    std::uint64_t fingerprint = 0;

    for (std::size_t index = chunk_min_size; index < target; ++index) {
        fingerprint = (fingerprint << 1U) + gear[std::to_integer<unsigned char>(input[index])];
        if ((fingerprint & early_mask) == 0) return index + 1;
    }
    for (std::size_t index = target; index < limit; ++index) {
        fingerprint = (fingerprint << 1U) + gear[std::to_integer<unsigned char>(input[index])];
        if ((fingerprint & late_mask) == 0) return index + 1;
    }
    return limit;
}

}  // namespace

std::vector<chunk_range> chunk(std::span<const std::byte> input) {
    std::vector<chunk_range> output;
    output.reserve(input.size() / chunk_target_size + 1);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto length = next_boundary(input.subspan(offset));
        output.push_back(chunk_range{offset, length});
        offset += length;
    }
    return output;
}

}  // namespace liquid::store_detail
