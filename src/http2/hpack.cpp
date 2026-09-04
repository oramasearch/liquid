#include "http2/hpack.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace liquid::http2 {
namespace {

struct static_entry {
    std::string_view name;
    std::string_view value;
};

constexpr auto static_table = std::array{
    static_entry{":authority", ""},
    static_entry{":method", "GET"},
    static_entry{":method", "POST"},
    static_entry{":path", "/"},
    static_entry{":path", "/index.html"},
    static_entry{":scheme", "http"},
    static_entry{":scheme", "https"},
    static_entry{":status", "200"},
    static_entry{":status", "204"},
    static_entry{":status", "206"},
    static_entry{":status", "304"},
    static_entry{":status", "400"},
    static_entry{":status", "404"},
    static_entry{":status", "500"},
    static_entry{"accept-charset", ""},
    static_entry{"accept-encoding", "gzip, deflate"},
    static_entry{"accept-language", ""},
    static_entry{"accept-ranges", ""},
    static_entry{"accept", ""},
    static_entry{"access-control-allow-origin", ""},
    static_entry{"age", ""},
    static_entry{"allow", ""},
    static_entry{"authorization", ""},
    static_entry{"cache-control", ""},
    static_entry{"content-disposition", ""},
    static_entry{"content-encoding", ""},
    static_entry{"content-language", ""},
    static_entry{"content-length", ""},
    static_entry{"content-location", ""},
    static_entry{"content-range", ""},
    static_entry{"content-type", ""},
    static_entry{"cookie", ""},
    static_entry{"date", ""},
    static_entry{"etag", ""},
    static_entry{"expect", ""},
    static_entry{"expires", ""},
    static_entry{"from", ""},
    static_entry{"host", ""},
    static_entry{"if-match", ""},
    static_entry{"if-modified-since", ""},
    static_entry{"if-none-match", ""},
    static_entry{"if-range", ""},
    static_entry{"if-unmodified-since", ""},
    static_entry{"last-modified", ""},
    static_entry{"link", ""},
    static_entry{"location", ""},
    static_entry{"max-forwards", ""},
    static_entry{"proxy-authenticate", ""},
    static_entry{"proxy-authorization", ""},
    static_entry{"range", ""},
    static_entry{"referer", ""},
    static_entry{"refresh", ""},
    static_entry{"retry-after", ""},
    static_entry{"server", ""},
    static_entry{"set-cookie", ""},
    static_entry{"strict-transport-security", ""},
    static_entry{"transfer-encoding", ""},
    static_entry{"user-agent", ""},
    static_entry{"vary", ""},
    static_entry{"via", ""},
    static_entry{"www-authenticate", ""},
};

struct huffman_code {
    std::uint32_t value;
    std::uint8_t bits;
};

constexpr auto huffman_codes = std::array{
    huffman_code{0x1ff8U, 13U},    huffman_code{0x7fffd8U, 23U},   huffman_code{0xfffffe2U, 28U},
    huffman_code{0xfffffe3U, 28U}, huffman_code{0xfffffe4U, 28U},  huffman_code{0xfffffe5U, 28U},
    huffman_code{0xfffffe6U, 28U}, huffman_code{0xfffffe7U, 28U},  huffman_code{0xfffffe8U, 28U},
    huffman_code{0xffffeaU, 24U},  huffman_code{0x3ffffffcU, 30U}, huffman_code{0xfffffe9U, 28U},
    huffman_code{0xfffffeaU, 28U}, huffman_code{0x3ffffffdU, 30U}, huffman_code{0xfffffebU, 28U},
    huffman_code{0xfffffecU, 28U}, huffman_code{0xfffffedU, 28U},  huffman_code{0xfffffeeU, 28U},
    huffman_code{0xfffffefU, 28U}, huffman_code{0xffffff0U, 28U},  huffman_code{0xffffff1U, 28U},
    huffman_code{0xffffff2U, 28U}, huffman_code{0x3ffffffeU, 30U}, huffman_code{0xffffff3U, 28U},
    huffman_code{0xffffff4U, 28U}, huffman_code{0xffffff5U, 28U},  huffman_code{0xffffff6U, 28U},
    huffman_code{0xffffff7U, 28U}, huffman_code{0xffffff8U, 28U},  huffman_code{0xffffff9U, 28U},
    huffman_code{0xffffffaU, 28U}, huffman_code{0xffffffbU, 28U},  huffman_code{0x14U, 6U},
    huffman_code{0x3f8U, 10U},     huffman_code{0x3f9U, 10U},      huffman_code{0xffaU, 12U},
    huffman_code{0x1ff9U, 13U},    huffman_code{0x15U, 6U},        huffman_code{0xf8U, 8U},
    huffman_code{0x7faU, 11U},     huffman_code{0x3faU, 10U},      huffman_code{0x3fbU, 10U},
    huffman_code{0xf9U, 8U},       huffman_code{0x7fbU, 11U},      huffman_code{0xfaU, 8U},
    huffman_code{0x16U, 6U},       huffman_code{0x17U, 6U},        huffman_code{0x18U, 6U},
    huffman_code{0x0U, 5U},        huffman_code{0x1U, 5U},         huffman_code{0x2U, 5U},
    huffman_code{0x19U, 6U},       huffman_code{0x1aU, 6U},        huffman_code{0x1bU, 6U},
    huffman_code{0x1cU, 6U},       huffman_code{0x1dU, 6U},        huffman_code{0x1eU, 6U},
    huffman_code{0x1fU, 6U},       huffman_code{0x5cU, 7U},        huffman_code{0xfbU, 8U},
    huffman_code{0x7ffcU, 15U},    huffman_code{0x20U, 6U},        huffman_code{0xffbU, 12U},
    huffman_code{0x3fcU, 10U},     huffman_code{0x1ffaU, 13U},     huffman_code{0x21U, 6U},
    huffman_code{0x5dU, 7U},       huffman_code{0x5eU, 7U},        huffman_code{0x5fU, 7U},
    huffman_code{0x60U, 7U},       huffman_code{0x61U, 7U},        huffman_code{0x62U, 7U},
    huffman_code{0x63U, 7U},       huffman_code{0x64U, 7U},        huffman_code{0x65U, 7U},
    huffman_code{0x66U, 7U},       huffman_code{0x67U, 7U},        huffman_code{0x68U, 7U},
    huffman_code{0x69U, 7U},       huffman_code{0x6aU, 7U},        huffman_code{0x6bU, 7U},
    huffman_code{0x6cU, 7U},       huffman_code{0x6dU, 7U},        huffman_code{0x6eU, 7U},
    huffman_code{0x6fU, 7U},       huffman_code{0x70U, 7U},        huffman_code{0x71U, 7U},
    huffman_code{0x72U, 7U},       huffman_code{0xfcU, 8U},        huffman_code{0x73U, 7U},
    huffman_code{0xfdU, 8U},       huffman_code{0x1ffbU, 13U},     huffman_code{0x7fff0U, 19U},
    huffman_code{0x1ffcU, 13U},    huffman_code{0x3ffcU, 14U},     huffman_code{0x22U, 6U},
    huffman_code{0x7ffdU, 15U},    huffman_code{0x3U, 5U},         huffman_code{0x23U, 6U},
    huffman_code{0x4U, 5U},        huffman_code{0x24U, 6U},        huffman_code{0x5U, 5U},
    huffman_code{0x25U, 6U},       huffman_code{0x26U, 6U},        huffman_code{0x27U, 6U},
    huffman_code{0x6U, 5U},        huffman_code{0x74U, 7U},        huffman_code{0x75U, 7U},
    huffman_code{0x28U, 6U},       huffman_code{0x29U, 6U},        huffman_code{0x2aU, 6U},
    huffman_code{0x7U, 5U},        huffman_code{0x2bU, 6U},        huffman_code{0x76U, 7U},
    huffman_code{0x2cU, 6U},       huffman_code{0x8U, 5U},         huffman_code{0x9U, 5U},
    huffman_code{0x2dU, 6U},       huffman_code{0x77U, 7U},        huffman_code{0x78U, 7U},
    huffman_code{0x79U, 7U},       huffman_code{0x7aU, 7U},        huffman_code{0x7bU, 7U},
    huffman_code{0x7ffeU, 15U},    huffman_code{0x7fcU, 11U},      huffman_code{0x3ffdU, 14U},
    huffman_code{0x1ffdU, 13U},    huffman_code{0xffffffcU, 28U},  huffman_code{0xfffe6U, 20U},
    huffman_code{0x3fffd2U, 22U},  huffman_code{0xfffe7U, 20U},    huffman_code{0xfffe8U, 20U},
    huffman_code{0x3fffd3U, 22U},  huffman_code{0x3fffd4U, 22U},   huffman_code{0x3fffd5U, 22U},
    huffman_code{0x7fffd9U, 23U},  huffman_code{0x3fffd6U, 22U},   huffman_code{0x7fffdaU, 23U},
    huffman_code{0x7fffdbU, 23U},  huffman_code{0x7fffdcU, 23U},   huffman_code{0x7fffddU, 23U},
    huffman_code{0x7fffdeU, 23U},  huffman_code{0xffffebU, 24U},   huffman_code{0x7fffdfU, 23U},
    huffman_code{0xffffecU, 24U},  huffman_code{0xffffedU, 24U},   huffman_code{0x3fffd7U, 22U},
    huffman_code{0x7fffe0U, 23U},  huffman_code{0xffffeeU, 24U},   huffman_code{0x7fffe1U, 23U},
    huffman_code{0x7fffe2U, 23U},  huffman_code{0x7fffe3U, 23U},   huffman_code{0x7fffe4U, 23U},
    huffman_code{0x1fffdcU, 21U},  huffman_code{0x3fffd8U, 22U},   huffman_code{0x7fffe5U, 23U},
    huffman_code{0x3fffd9U, 22U},  huffman_code{0x7fffe6U, 23U},   huffman_code{0x7fffe7U, 23U},
    huffman_code{0xffffefU, 24U},  huffman_code{0x3fffdaU, 22U},   huffman_code{0x1fffddU, 21U},
    huffman_code{0xfffe9U, 20U},   huffman_code{0x3fffdbU, 22U},   huffman_code{0x3fffdcU, 22U},
    huffman_code{0x7fffe8U, 23U},  huffman_code{0x7fffe9U, 23U},   huffman_code{0x1fffdeU, 21U},
    huffman_code{0x7fffeaU, 23U},  huffman_code{0x3fffddU, 22U},   huffman_code{0x3fffdeU, 22U},
    huffman_code{0xfffff0U, 24U},  huffman_code{0x1fffdfU, 21U},   huffman_code{0x3fffdfU, 22U},
    huffman_code{0x7fffebU, 23U},  huffman_code{0x7fffecU, 23U},   huffman_code{0x1fffe0U, 21U},
    huffman_code{0x1fffe1U, 21U},  huffman_code{0x3fffe0U, 22U},   huffman_code{0x1fffe2U, 21U},
    huffman_code{0x7fffedU, 23U},  huffman_code{0x3fffe1U, 22U},   huffman_code{0x7fffeeU, 23U},
    huffman_code{0x7fffefU, 23U},  huffman_code{0xfffeaU, 20U},    huffman_code{0x3fffe2U, 22U},
    huffman_code{0x3fffe3U, 22U},  huffman_code{0x3fffe4U, 22U},   huffman_code{0x7ffff0U, 23U},
    huffman_code{0x3fffe5U, 22U},  huffman_code{0x3fffe6U, 22U},   huffman_code{0x7ffff1U, 23U},
    huffman_code{0x3ffffe0U, 26U}, huffman_code{0x3ffffe1U, 26U},  huffman_code{0xfffebU, 20U},
    huffman_code{0x7fff1U, 19U},   huffman_code{0x3fffe7U, 22U},   huffman_code{0x7ffff2U, 23U},
    huffman_code{0x3fffe8U, 22U},  huffman_code{0x1ffffecU, 25U},  huffman_code{0x3ffffe2U, 26U},
    huffman_code{0x3ffffe3U, 26U}, huffman_code{0x3ffffe4U, 26U},  huffman_code{0x7ffffdeU, 27U},
    huffman_code{0x7ffffdfU, 27U}, huffman_code{0x3ffffe5U, 26U},  huffman_code{0xfffff1U, 24U},
    huffman_code{0x1ffffedU, 25U}, huffman_code{0x7fff2U, 19U},    huffman_code{0x1fffe3U, 21U},
    huffman_code{0x3ffffe6U, 26U}, huffman_code{0x7ffffe0U, 27U},  huffman_code{0x7ffffe1U, 27U},
    huffman_code{0x3ffffe7U, 26U}, huffman_code{0x7ffffe2U, 27U},  huffman_code{0xfffff2U, 24U},
    huffman_code{0x1fffe4U, 21U},  huffman_code{0x1fffe5U, 21U},   huffman_code{0x3ffffe8U, 26U},
    huffman_code{0x3ffffe9U, 26U}, huffman_code{0xffffffdU, 28U},  huffman_code{0x7ffffe3U, 27U},
    huffman_code{0x7ffffe4U, 27U}, huffman_code{0x7ffffe5U, 27U},  huffman_code{0xfffecU, 20U},
    huffman_code{0xfffff3U, 24U},  huffman_code{0xfffedU, 20U},    huffman_code{0x1fffe6U, 21U},
    huffman_code{0x3fffe9U, 22U},  huffman_code{0x1fffe7U, 21U},   huffman_code{0x1fffe8U, 21U},
    huffman_code{0x7ffff3U, 23U},  huffman_code{0x3fffeaU, 22U},   huffman_code{0x3fffebU, 22U},
    huffman_code{0x1ffffeeU, 25U}, huffman_code{0x1ffffefU, 25U},  huffman_code{0xfffff4U, 24U},
    huffman_code{0xfffff5U, 24U},  huffman_code{0x3ffffeaU, 26U},  huffman_code{0x7ffff4U, 23U},
    huffman_code{0x3ffffebU, 26U}, huffman_code{0x7ffffe6U, 27U},  huffman_code{0x3ffffecU, 26U},
    huffman_code{0x3ffffedU, 26U}, huffman_code{0x7ffffe7U, 27U},  huffman_code{0x7ffffe8U, 27U},
    huffman_code{0x7ffffe9U, 27U}, huffman_code{0x7ffffeaU, 27U},  huffman_code{0x7ffffebU, 27U},
    huffman_code{0xffffffeU, 28U}, huffman_code{0x7ffffecU, 27U},  huffman_code{0x7ffffedU, 27U},
    huffman_code{0x7ffffeeU, 27U}, huffman_code{0x7ffffefU, 27U},  huffman_code{0x7fffff0U, 27U},
    huffman_code{0x3ffffeeU, 26U},
};

struct huffman_node {
    std::array<int, 2> child{-1, -1};
    int symbol{-1};
};

auto fail(std::string message) -> error {
    return {errc::corrupt, std::move(message), "HPACK"};
}

auto byte_value(std::byte value) -> std::uint8_t {
    return std::to_integer<std::uint8_t>(value);
}

void encode_integer(std::vector<std::byte>& output,
                    std::uint8_t prefix,
                    std::uint8_t first_bits,
                    std::size_t value) {
    const auto limit = static_cast<std::size_t>((1U << prefix) - 1U);
    if (value < limit) {
        output.push_back(static_cast<std::byte>(first_bits | value));
        return;
    }
    output.push_back(static_cast<std::byte>(first_bits | limit));
    value -= limit;
    while (value >= 128) {
        output.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    output.push_back(static_cast<std::byte>(value));
}

auto decode_integer(std::span<const std::byte> input, std::size_t& offset, std::uint8_t prefix)
    -> result<std::size_t> {
    if (offset == input.size()) {
        return std::unexpected(fail("integer is incomplete"));
    }
    const auto limit = static_cast<std::size_t>((1U << prefix) - 1U);
    auto value = static_cast<std::size_t>(byte_value(input[offset]) & limit);
    ++offset;
    if (value < limit) {
        return value;
    }
    auto shift = unsigned{};
    while (offset < input.size()) {
        const auto next = byte_value(input[offset]);
        ++offset;
        if (shift >= std::numeric_limits<std::size_t>::digits ||
            (static_cast<std::size_t>(next & 0x7fU) << std::min(shift, unsigned{63})) >
                std::numeric_limits<std::size_t>::max() - value) {
            return std::unexpected(fail("integer overflows"));
        }
        value += static_cast<std::size_t>(next & 0x7fU) << shift;
        if ((next & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    return std::unexpected(fail("integer continuation is incomplete"));
}

void encode_string(std::vector<std::byte>& output, std::string_view value) {
    const auto encoded = huffman_encode(value);
    if (encoded.size() < value.size()) {
        encode_integer(output, 7, 0x80U, encoded.size());
        output.insert(output.end(), encoded.begin(), encoded.end());
        return;
    }
    encode_integer(output, 7, 0, value.size());
    for (const auto character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

auto decode_string(std::span<const std::byte> input, std::size_t& offset) -> result<std::string> {
    if (offset == input.size()) {
        return std::unexpected(fail("string is incomplete"));
    }
    const auto encoded = (byte_value(input[offset]) & 0x80U) != 0;
    auto size = decode_integer(input, offset, 7);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > input.size() - offset) {
        return std::unexpected(fail("string body is incomplete"));
    }
    const auto body = input.subspan(offset, *size);
    offset += *size;
    if (encoded) {
        return huffman_decode(body);
    }
    auto output = std::string{};
    output.reserve(*size);
    for (const auto byte : body) {
        output.push_back(static_cast<char>(byte_value(byte)));
    }
    return output;
}

template <class Entries>
auto find_entry(const Entries& dynamic, std::string_view name, std::string_view value)
    -> std::size_t {
    for (std::size_t index = 0; index < static_table.size(); ++index) {
        if (static_table[index].name == name && static_table[index].value == value) {
            return index + 1;
        }
    }
    for (std::size_t index = 0; index < dynamic.size(); ++index) {
        if (dynamic[index].field.name == name && dynamic[index].field.value == value) {
            return static_table.size() + index + 1;
        }
    }
    return 0;
}

template <class Entries>
auto find_name(const Entries& dynamic, std::string_view name) -> std::size_t {
    for (std::size_t index = 0; index < static_table.size(); ++index) {
        if (static_table[index].name == name) {
            return index + 1;
        }
    }
    for (std::size_t index = 0; index < dynamic.size(); ++index) {
        if (dynamic[index].field.name == name) {
            return static_table.size() + index + 1;
        }
    }
    return 0;
}

template <class Entries>
auto field_at(const Entries& dynamic, std::size_t index) -> result<header_field> {
    if (index == 0) {
        return std::unexpected(fail("header index is zero"));
    }
    if (index <= static_table.size()) {
        const auto& entry = static_table[index - 1];
        return header_field{std::string(entry.name), std::string(entry.value), false};
    }
    const auto dynamic_index = index - static_table.size() - 1;
    if (dynamic_index >= dynamic.size()) {
        return std::unexpected(fail("header index is outside the dynamic table"));
    }
    return dynamic[dynamic_index].field;
}

auto entry_size(const header_field& field) -> std::size_t {
    return field.name.size() + field.value.size() + 32;
}

template <class Entries>
void insert_entry(Entries& dynamic, std::size_t& size, std::size_t max_size, header_field field) {
    const auto required = entry_size(field);
    if (required > max_size) {
        dynamic.clear();
        size = 0;
        return;
    }
    while (!dynamic.empty() && size + required > max_size) {
        size -= entry_size(dynamic.back().field);
        dynamic.pop_back();
    }
    size += required;
    dynamic.insert(dynamic.begin(), typename Entries::value_type{std::move(field)});
}

template <class Entries>
void resize_entries(Entries& dynamic, std::size_t& size, std::size_t max_size) {
    while (!dynamic.empty() && size > max_size) {
        size -= entry_size(dynamic.back().field);
        dynamic.pop_back();
    }
}

} // namespace

auto huffman_encode(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    auto bits = std::uint64_t{};
    auto count = unsigned{};
    for (const auto character : input) {
        const auto code = huffman_codes[static_cast<unsigned char>(character)];
        bits = (bits << code.bits) | code.value;
        count += code.bits;
        while (count >= 8) {
            output.push_back(static_cast<std::byte>((bits >> (count - 8)) & 0xffU));
            count -= 8;
            if (count == 0) {
                bits = 0;
            } else {
                bits &= (std::uint64_t{1} << count) - 1;
            }
        }
    }
    if (count != 0) {
        const auto padding = 8U - count;
        bits = (bits << padding) | ((std::uint64_t{1} << padding) - 1);
        output.push_back(static_cast<std::byte>(bits & 0xffU));
    }
    return output;
}

auto huffman_decode(std::span<const std::byte> input) -> result<std::string> {
    auto nodes = std::vector<huffman_node>{1};
    for (std::size_t symbol = 0; symbol < huffman_codes.size(); ++symbol) {
        auto node = 0;
        const auto code = huffman_codes[symbol];
        for (auto bit_index = int(code.bits) - 1; bit_index >= 0; --bit_index) {
            const auto bit = static_cast<unsigned>((code.value >> bit_index) & 1U);
            if (nodes[node].child[bit] < 0) {
                nodes[node].child[bit] = static_cast<int>(nodes.size());
                nodes.push_back(huffman_node{});
            }
            node = nodes[node].child[bit];
        }
        nodes[node].symbol = static_cast<int>(symbol);
    }

    auto output = std::string{};
    auto node = 0;
    auto trailing = unsigned{};
    auto trailing_value = std::uint64_t{};
    for (const auto byte : input) {
        for (auto bit_index = 7; bit_index >= 0; --bit_index) {
            const auto bit = static_cast<unsigned>((byte_value(byte) >> bit_index) & 1U);
            node = nodes[node].child[bit];
            if (node < 0) {
                return std::unexpected(fail("Huffman code is invalid"));
            }
            ++trailing;
            trailing_value = (trailing_value << 1) | bit;
            if (nodes[node].symbol >= 0) {
                output.push_back(static_cast<char>(nodes[node].symbol));
                node = 0;
                trailing = 0;
                trailing_value = 0;
            }
        }
    }
    if (trailing > 7 || (trailing != 0 && trailing_value != (std::uint64_t{1} << trailing) - 1)) {
        return std::unexpected(fail("Huffman padding is invalid"));
    }
    return output;
}

hpack_encoder::hpack_encoder(std::size_t table_size) : max_size_(table_size) {}

void hpack_encoder::set_max_table_size(std::size_t size) {
    max_size_ = size;
    resize_entries(dynamic_, size_, max_size_);
    emit_size_update_ = true;
}

void hpack_encoder::insert(header_field field) {
    insert_entry(dynamic_, size_, max_size_, std::move(field));
}

auto hpack_encoder::encode(std::span<const header_field> fields) -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    if (emit_size_update_) {
        encode_integer(output, 5, 0x20U, max_size_);
        emit_size_update_ = false;
    }
    for (const auto& field : fields) {
        const auto exact = find_entry(dynamic_, field.name, field.value);
        if (exact != 0) {
            encode_integer(output, 7, 0x80U, exact);
            continue;
        }
        const auto name = find_name(dynamic_, field.name);
        if (field.sensitive) {
            encode_integer(output, 4, 0x10U, name);
            if (name == 0) {
                encode_string(output, field.name);
            }
            encode_string(output, field.value);
            continue;
        }
        encode_integer(output, 6, 0x40U, name);
        if (name == 0) {
            encode_string(output, field.name);
        }
        encode_string(output, field.value);
        insert(field);
    }
    return output;
}

hpack_decoder::hpack_decoder(std::size_t table_size) : max_size_(table_size) {}

void hpack_decoder::set_max_table_size(std::size_t size) {
    max_size_ = size;
    resize_entries(dynamic_, size_, max_size_);
}

void hpack_decoder::insert(header_field field) {
    insert_entry(dynamic_, size_, max_size_, std::move(field));
}

auto hpack_decoder::decode(std::span<const std::byte> block) -> result<std::vector<header_field>> {
    auto output = std::vector<header_field>{};
    auto offset = std::size_t{};
    auto allow_size_update = true;
    while (offset < block.size()) {
        const auto first = byte_value(block[offset]);
        if ((first & 0x80U) != 0) {
            auto index = decode_integer(block, offset, 7);
            if (!index) {
                return std::unexpected(index.error());
            }
            auto field = field_at(dynamic_, *index);
            if (!field) {
                return std::unexpected(field.error());
            }
            output.push_back(std::move(*field));
            allow_size_update = false;
            continue;
        }
        if ((first & 0xe0U) == 0x20U) {
            if (!allow_size_update) {
                return std::unexpected(fail("dynamic table size update follows a header"));
            }
            auto size = decode_integer(block, offset, 5);
            if (!size) {
                return std::unexpected(size.error());
            }
            if (*size > max_size_) {
                return std::unexpected(fail("dynamic table size exceeds configured maximum"));
            }
            max_size_ = *size;
            resize_entries(dynamic_, size_, max_size_);
            continue;
        }
        const auto incremental = (first & 0x40U) != 0;
        const auto never_indexed = !incremental && (first & 0xf0U) == 0x10U;
        const auto prefix = incremental ? 6U : 4U;
        auto name_index = decode_integer(block, offset, prefix);
        if (!name_index) {
            return std::unexpected(name_index.error());
        }
        auto name = std::string{};
        if (*name_index == 0) {
            auto literal = decode_string(block, offset);
            if (!literal) {
                return std::unexpected(literal.error());
            }
            name = std::move(*literal);
        } else {
            auto indexed = field_at(dynamic_, *name_index);
            if (!indexed) {
                return std::unexpected(indexed.error());
            }
            name = std::move(indexed->name);
        }
        auto value = decode_string(block, offset);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto field = header_field{std::move(name), std::move(*value), never_indexed};
        output.push_back(field);
        if (incremental) {
            insert(std::move(field));
        }
        allow_size_update = false;
    }
    return output;
}

} // namespace liquid::http2
