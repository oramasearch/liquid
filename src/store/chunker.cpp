#include "chunker.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace liquid::store_detail {
namespace {

constexpr std::array<std::uint64_t, 256> make_gear() {
  std::array<std::uint64_t, 256> table{};
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (auto &value : table) {
    state ^= state >> 12U;
    state ^= state << 25U;
    state ^= state >> 27U;
    value = state * 0x2545f4914f6cdd1dULL;
  }
  return table;
}

constexpr auto gear = make_gear();
constexpr std::uint64_t small_mask = (std::uint64_t{1} << 15U) - 1U;
constexpr std::uint64_t large_mask = (std::uint64_t{1} << 17U) - 1U;

std::size_t next_cut(std::span<const std::byte> input, std::size_t start) {
  const auto remaining = input.size() - start;
  if (remaining <= chunk_minimum)
    return input.size();

  const auto end = std::min(input.size(), start + chunk_maximum);
  const auto normal = std::min(end, start + chunk_target);
  std::uint64_t fingerprint = 0;
  for (std::size_t at = start + chunk_minimum; at < end; ++at) {
    fingerprint = (fingerprint << 1U) + gear[std::to_integer<unsigned char>(input[at])];
    const auto mask = at < normal ? small_mask : large_mask;
    if ((fingerprint & mask) == 0)
      return at + 1U;
  }
  return end;
}

} // namespace

std::vector<chunk_range> fastcdc(std::span<const std::byte> input) {
  std::vector<chunk_range> output;
  if (input.empty()) {
    output.push_back({0, 0});
    return output;
  }
  for (std::size_t offset = 0; offset < input.size();) {
    const auto end = next_cut(input, offset);
    output.push_back({offset, end - offset});
    offset = end;
  }
  return output;
}

} // namespace liquid::store_detail
