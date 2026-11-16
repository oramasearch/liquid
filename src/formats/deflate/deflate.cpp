// SPDX-License-Identifier: MIT

#include "formats/deflate/deflate.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include <zlib.h>

namespace liquid::formats::deflate {
namespace {

auto crc32_bytes(std::span<const std::byte> input) -> std::uint32_t {
    auto crc = std::uint32_t{0xffffffffU};
    for (const auto byte : input) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (auto bit = 0; bit < 8; ++bit) {
            const auto mask = std::uint32_t{0} - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8U)});
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (auto index = 0U; index < 4; ++index) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> (index * 8U))});
    }
}

auto corrupt(std::string message) -> error {
    return {errc::corrupt, std::move(message), "gzip"};
}

} // namespace

std::vector<std::byte> gzip(std::span<const std::byte> input) {
    auto output = std::vector<std::byte>{std::byte{0x1f},
                                         std::byte{0x8b},
                                         std::byte{0x08},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{0x00},
                                         std::byte{0xff}};
    auto offset = std::size_t{};
    do {
        const auto available = input.size() - offset;
        const auto count = std::min<std::size_t>(available, 65535);
        const auto final = offset + count == input.size();
        output.push_back(final ? std::byte{0x01} : std::byte{0x00});
        append_u16(output, static_cast<std::uint16_t>(count));
        append_u16(output, static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count)));
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(offset),
                      input.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    } while (offset < input.size());
    append_u32(output, crc32_bytes(input));
    append_u32(output, static_cast<std::uint32_t>(input.size()));
    return output;
}

result<std::pair<std::vector<std::byte>, std::size_t>> gunzip(std::span<const std::byte> input,
                                                              std::uint64_t maximum_output) {
    if (input.size() < 18 || input[0] != std::byte{0x1f} || input[1] != std::byte{0x8b} ||
        input[2] != std::byte{0x08} || input[3] != std::byte{0x00}) {
        return std::unexpected(corrupt("invalid deterministic gzip header"));
    }
    if (input.size() > std::numeric_limits<uInt>::max()) {
        return std::unexpected(error{errc::unsupported, "gzip member exceeds zlib limits", "gzip"});
    }
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return std::unexpected(error{errc::internal, "cannot initialize gzip decoder", "gzip"});
    }
    auto output = std::vector<std::byte>{};
    auto chunk = std::array<std::byte, 64 * 1024>{};
    auto status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = chunk.size() - stream.avail_out;
        if (produced > maximum_output || output.size() > maximum_output - produced) {
            inflateEnd(&stream);
            return std::unexpected(
                error{errc::unsupported, "gzip member exceeds the decoded byte limit", "gzip"});
        }
        output.insert(output.end(), chunk.begin(), chunk.end() - stream.avail_out);
    }
    const auto consumed = static_cast<std::size_t>(stream.total_in);
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return std::unexpected(corrupt("invalid gzip member"));
    }
    return std::pair{std::move(output), consumed};
}

} // namespace liquid::formats::deflate
