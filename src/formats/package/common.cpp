#include "formats/package/common.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <memory>

#include <zstd.h>

#include "encoding/cbor.hpp"

namespace liquid::formats::package {
namespace {

constexpr auto md5_shift = std::to_array<unsigned>(
    {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
     14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
     4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21});

constexpr auto md5_constant = std::to_array<std::uint32_t>(
    {0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
     0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
     0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
     0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
     0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
     0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
     0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
     0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
     0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
     0xeb86d391U});

void append_hex(std::vector<std::byte>& output, std::uint64_t value, std::size_t width) {
    auto buffer = std::array<char, 32>{};
    std::ranges::fill(buffer, '0');
    auto converted = std::array<char, 32>{};
    const auto [end, code] =
        std::to_chars(converted.data(), converted.data() + converted.size(), value, 16);
    const auto count = code == std::errc{} ? static_cast<std::size_t>(end - converted.data()) : 0;
    const auto begin = width - std::min(width, count);
    std::copy(converted.begin() + static_cast<std::ptrdiff_t>(count - (width - begin)),
              converted.begin() + static_cast<std::ptrdiff_t>(count),
              buffer.begin() + static_cast<std::ptrdiff_t>(begin));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(buffer.data()),
                  reinterpret_cast<const std::byte*>(buffer.data() + width));
}

void pad_four(std::vector<std::byte>& output) {
    while (output.size() % 4 != 0) {
        output.push_back(std::byte{});
    }
}

} // namespace

std::vector<std::byte> text_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

result<digest> sha256(std::span<const std::byte> input) {
    return hash(algorithm::sha256, input);
}

std::string hex_bytes(std::span<const std::byte> input) {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto value : input) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

std::string md5_hex(std::span<const std::byte> input) {
    auto bytes = std::vector<std::byte>{input.begin(), input.end()};
    const auto bit_count = static_cast<std::uint64_t>(input.size()) * 8U;
    bytes.push_back(std::byte{0x80});
    while (bytes.size() % 64 != 56) {
        bytes.push_back(std::byte{});
    }
    for (auto index = 0U; index < 8; ++index) {
        bytes.push_back(std::byte{static_cast<std::uint8_t>(bit_count >> (index * 8U))});
    }
    auto a0 = std::uint32_t{0x67452301U};
    auto b0 = std::uint32_t{0xefcdab89U};
    auto c0 = std::uint32_t{0x98badcfeU};
    auto d0 = std::uint32_t{0x10325476U};
    for (auto offset = std::size_t{}; offset < bytes.size(); offset += 64) {
        auto words = std::array<std::uint32_t, 16>{};
        for (auto word = 0U; word < words.size(); ++word) {
            for (auto byte = 0U; byte < 4; ++byte) {
                words[word] |=
                    std::uint32_t{std::to_integer<std::uint8_t>(bytes[offset + word * 4 + byte])}
                    << (byte * 8U);
            }
        }
        auto a = a0;
        auto b = b0;
        auto c = c0;
        auto d = d0;
        for (auto index = 0U; index < 64; ++index) {
            auto function = std::uint32_t{};
            auto word = 0U;
            if (index < 16) {
                function = (b & c) | (~b & d);
                word = index;
            } else if (index < 32) {
                function = (d & b) | (~d & c);
                word = (5 * index + 1) % 16;
            } else if (index < 48) {
                function = b ^ c ^ d;
                word = (3 * index + 5) % 16;
            } else {
                function = c ^ (b | ~d);
                word = (7 * index) % 16;
            }
            const auto next = d;
            d = c;
            c = b;
            b += std::rotl(a + function + md5_constant[index] + words[word], md5_shift[index]);
            a = next;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }
    auto digest_bytes = std::array<std::byte, 16>{};
    for (auto index = 0U; index < 4; ++index) {
        const auto value = std::array{a0, b0, c0, d0}[index];
        for (auto byte = 0U; byte < 4; ++byte) {
            digest_bytes[index * 4 + byte] =
                std::byte{static_cast<std::uint8_t>(value >> (byte * 8U))};
        }
    }
    return hex_bytes(digest_bytes);
}

result<std::vector<std::byte>> zstd(std::span<const std::byte> input) {
    auto context =
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>{ZSTD_createCCtx(), ZSTD_freeCCtx};
    if (!context) {
        return std::unexpected(error{errc::internal, "cannot create zstd context", "projection"});
    }
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, 1);
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_nbWorkers, 0);
    auto output = std::vector<std::byte>(ZSTD_compressBound(input.size()));
    const auto size =
        ZSTD_compress2(context.get(), output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(size)) {
        return std::unexpected(error{errc::internal, ZSTD_getErrorName(size), "projection zstd"});
    }
    output.resize(size);
    return output;
}

result<std::vector<std::byte>>
tar(std::span<const file> files, std::uint64_t source_date_epoch, std::string_view prefix) {
    auto entries = std::vector<formats::tar::entry>{};
    entries.reserve(files.size());
    for (const auto& value : files) {
        auto path = prefix.empty() ? value.path : std::string{prefix} + "/" + value.path;
        entries.push_back(
            {std::move(path), value.bytes, value.mode & 0111U ? 0755U : 0644U, false});
    }
    return formats::tar::write(entries, source_date_epoch);
}

result<std::vector<std::byte>> cpio(std::span<const file> files, std::uint64_t source_date_epoch) {
    auto sorted = std::vector<file>{files.begin(), files.end()};
    std::ranges::sort(sorted, {}, &file::path);
    auto output = std::vector<std::byte>{};
    auto inode = std::uint64_t{1};
    const auto append = [&](std::string name,
                            std::span<const std::byte> bytes,
                            std::uint32_t mode,
                            std::uint64_t file_inode) {
        const auto full_name = name == "TRAILER!!!" ? name : "./usr/" + name;
        output.insert(output.end(),
                      {std::byte{'0'},
                       std::byte{'7'},
                       std::byte{'0'},
                       std::byte{'7'},
                       std::byte{'0'},
                       std::byte{'1'}});
        append_hex(output, file_inode, 8);
        append_hex(output, 0100000U | mode, 8);
        append_hex(output, 0, 8);
        append_hex(output, 0, 8);
        append_hex(output, 1, 8);
        append_hex(output, source_date_epoch, 8);
        append_hex(output, bytes.size(), 8);
        append_hex(output, 0, 8);
        append_hex(output, 0, 8);
        append_hex(output, 0, 8);
        append_hex(output, 0, 8);
        append_hex(output, full_name.size() + 1, 8);
        append_hex(output, 0, 8);
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(full_name.data()),
                      reinterpret_cast<const std::byte*>(full_name.data() + full_name.size()));
        output.push_back(std::byte{});
        pad_four(output);
        output.insert(output.end(), bytes.begin(), bytes.end());
        pad_four(output);
    };
    for (const auto& value : sorted) {
        append(value.path, value.bytes, value.mode & 0111U ? 0755U : 0644U, inode++);
    }
    append("TRAILER!!!", {}, 0, 0);
    return output;
}

std::string join(std::span<const std::string> values, std::string_view separator) {
    auto output = std::string{};
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (index != 0) {
            output += separator;
        }
        output += values[index];
    }
    return output;
}

result<std::string> attribute(std::span<const std::byte> attributes, std::string_view name) {
    auto decoded = encoding::cbor::decode(attributes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* fields = std::get_if<encoding::cbor::value::map>(&decoded->data);
    if (!fields) {
        return std::unexpected(error{errc::corrupt, "package attributes are not a map", {}});
    }
    for (const auto& [key, value] : *fields) {
        const auto* key_text = std::get_if<std::string>(&key.data);
        if (!key_text || *key_text != name) {
            continue;
        }
        const auto* output = std::get_if<std::string>(&value.data);
        if (!output) {
            return std::unexpected(
                error{errc::corrupt, "package attribute is not text", std::string{name}});
        }
        return *output;
    }
    return std::unexpected(
        error{errc::not_found, "package attribute is absent", std::string{name}});
}

} // namespace liquid::formats::package
