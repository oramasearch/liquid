// SPDX-License-Identifier: MIT

#include "git/sha1.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace liquid::git_detail {
namespace {

auto rotate_left(std::uint32_t value, unsigned count) -> std::uint32_t {
    return (value << count) | (value >> (32U - count));
}

auto hex_digit(unsigned value) -> char {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

auto hex_value(char value) -> std::optional<unsigned> {
    if (value >= '0' && value <= '9')
        return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<unsigned>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return static_cast<unsigned>(value - 'A' + 10);
    return std::nullopt;
}

} // namespace

auto sha1_hash(std::span<const std::byte> input) -> sha1 {
    std::array<std::uint32_t, 5> state{
        0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U, 0xc3d2e1f0U};
    std::vector<std::byte> padded{input.begin(), input.end()};
    padded.push_back(std::byte{0x80});
    while ((padded.size() % 64U) != 56U)
        padded.push_back(std::byte{});
    const auto bits = static_cast<std::uint64_t>(input.size()) * 8U;
    for (int shift = 56; shift >= 0; shift -= 8)
        padded.push_back(
            std::byte{static_cast<unsigned char>(bits >> static_cast<unsigned>(shift))});
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto base = offset + index * 4;
            words[index] = (static_cast<std::uint32_t>(padded[base]) << 24U) |
                           (static_cast<std::uint32_t>(padded[base + 1]) << 16U) |
                           (static_cast<std::uint32_t>(padded[base + 2]) << 8U) |
                           static_cast<std::uint32_t>(padded[base + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index)
            words[index] = rotate_left(
                words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        auto a = state[0];
        auto b = state[1];
        auto c = state[2];
        auto d = state[3];
        auto e = state[4];
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint32_t function{};
            std::uint32_t constant{};
            if (index < 20) {
                function = (b & c) | ((~b) & d);
                constant = 0x5a827999U;
            } else if (index < 40) {
                function = b ^ c ^ d;
                constant = 0x6ed9eba1U;
            } else if (index < 60) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8f1bbcdcU;
            } else {
                function = b ^ c ^ d;
                constant = 0xca62c1d6U;
            }
            const auto next = rotate_left(a, 5) + function + e + constant + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }
    sha1 output{};
    for (std::size_t index = 0; index < state.size(); ++index)
        for (std::size_t byte = 0; byte < 4; ++byte)
            output[index * 4 + byte] =
                std::byte{static_cast<unsigned char>(state[index] >> (24U - byte * 8U))};
    return output;
}

auto sha1_hex(const sha1& value) -> std::string {
    std::string output;
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto number = std::to_integer<unsigned char>(byte);
        output.push_back(hex_digit(number >> 4U));
        output.push_back(hex_digit(number & 0x0fU));
    }
    return output;
}

auto parse_sha1(std::string_view text) -> std::optional<sha1> {
    if (text.size() != 40)
        return std::nullopt;
    sha1 output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto high = hex_value(text[index * 2]);
        const auto low = hex_value(text[index * 2 + 1]);
        if (!high || !low)
            return std::nullopt;
        output[index] = std::byte{static_cast<unsigned char>((*high << 4U) | *low)};
    }
    return output;
}

} // namespace liquid::git_detail
