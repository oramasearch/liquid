#pragma once

// Shared helpers for the libFuzzer harnesses in this directory (liquid #407).
//
// Inputs that reach more than one decoder start with a selector byte: the harness takes it modulo
// its decoder count, so every byte value is meaningful and the fuzzer can move between decoders.
// Inputs that carry several parts (an identity chain, a delta and its base) use two-byte
// big-endian length prefixes. The seed generator, seeds.cpp, writes inputs in the same layout.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace liquid::fuzz {

[[nodiscard]] inline auto bytes(const std::uint8_t* data, std::size_t size)
    -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(data), size};
}

[[nodiscard]] inline auto text(std::span<const std::byte> input) -> std::string_view {
    return {reinterpret_cast<const char*>(input.data()), input.size()};
}

struct selected {
    std::size_t decoder{};
    std::span<const std::byte> payload;
};

[[nodiscard]] inline auto select(std::span<const std::byte> input, std::size_t decoders)
    -> std::optional<selected> {
    if (input.empty() || decoders == 0) {
        return std::nullopt;
    }
    return selected{std::to_integer<std::size_t>(input.front()) % decoders, input.subspan(1)};
}

// Splits two-byte length-prefixed parts; a truncated last part is kept as it is.
[[nodiscard]] inline auto parts(std::span<const std::byte> input, std::size_t limit)
    -> std::vector<std::span<const std::byte>> {
    auto output = std::vector<std::span<const std::byte>>{};
    while (input.size() >= 2 && output.size() < limit) {
        const auto length =
            (std::to_integer<std::size_t>(input[0]) << 8U) | std::to_integer<std::size_t>(input[1]);
        input = input.subspan(2);
        const auto taken = length < input.size() ? length : input.size();
        output.push_back(input.first(taken));
        input = input.subspan(taken);
    }
    return output;
}

// Stops the process when a property the harness checks does not hold, as a sanitizer finding
// would. libFuzzer records the input as a crash.
inline void require(bool condition) {
    if (!condition) {
        __builtin_trap();
    }
}

} // namespace liquid::fuzz
