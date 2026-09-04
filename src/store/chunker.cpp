#include "chunker.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace liquid::store_detail {
namespace {

constexpr std::array<std::uint64_t, 256> make_gear() {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (auto& value : table) {
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

chunker::chunker() {
    current_.reserve(chunk_maximum);
}

bool chunker::push(std::byte value) {
    current_.push_back(value);
    const auto at = current_.size() - 1;
    if (at < chunk_minimum)
        return false;
    fingerprint_ = (fingerprint_ << 1U) + gear[std::to_integer<unsigned char>(current_.back())];
    const auto mask = at < chunk_target ? small_mask : large_mask;
    return (fingerprint_ & mask) == 0 || current_.size() == chunk_maximum;
}

std::vector<std::vector<std::byte>> chunker::feed(std::span<const std::byte> input) {
    auto output = std::vector<std::vector<std::byte>>{};
    if (finished_)
        return output;
    for (const auto value : input) {
        if (!push(value))
            continue;
        output.push_back(std::move(current_));
        current_.clear();
        current_.reserve(chunk_maximum);
        fingerprint_ = 0;
        emitted_ = true;
    }
    return output;
}

std::vector<std::vector<std::byte>> chunker::finish() {
    auto output = std::vector<std::vector<std::byte>>{};
    if (finished_)
        return output;
    finished_ = true;
    if (!current_.empty() || !emitted_)
        output.push_back(std::move(current_));
    return output;
}

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
