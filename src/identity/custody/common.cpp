// SPDX-License-Identifier: MIT

#include "identity/custody/common.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "hash/sha256.h"
#include "monocypher.h"

namespace liquid::custody_detail {
namespace {

constexpr auto base64_alphabet =
    std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
constexpr auto base64_url_alphabet =
    std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};

auto base64_value(char character, bool url) -> int {
    const auto alphabet = url ? base64_url_alphabet : base64_alphabet;
    const auto found = alphabet.find(character);
    return found == std::string_view::npos ? -1 : static_cast<int>(found);
}

void put_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (auto shift = 0U; shift != 64U; shift += 8U) {
        output.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

auto load_u32(const std::byte* input) -> std::uint32_t {
    auto output = std::uint32_t{};
    for (auto index = 0U; index != 4U; ++index) {
        output |= std::uint32_t{std::to_integer<unsigned char>(input[index])} << (index * 8U);
    }
    return output;
}

void store_u32(std::byte* output, std::uint32_t value) {
    for (auto index = 0U; index != 4U; ++index) {
        output[index] = std::byte{static_cast<unsigned char>(value >> (index * 8U))};
    }
}

void quarter_round(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) {
    a += b;
    d ^= a;
    d = std::rotl(d, 16);
    c += d;
    b ^= c;
    b = std::rotl(b, 12);
    a += b;
    d ^= a;
    d = std::rotl(d, 8);
    c += d;
    b ^= c;
    b = std::rotl(b, 7);
}

auto chacha_block(std::span<const std::byte, 32> key_bytes,
                  std::uint32_t counter,
                  std::span<const std::byte, 12> nonce) -> std::array<std::byte, 64> {
    auto state = std::array<std::uint32_t, 16>{0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
    for (auto index = std::size_t{}; index != 8; ++index) {
        state[4 + index] = load_u32(key_bytes.data() + index * 4);
    }
    state[12] = counter;
    state[13] = load_u32(nonce.data());
    state[14] = load_u32(nonce.data() + 4);
    state[15] = load_u32(nonce.data() + 8);
    auto working = state;
    for (auto round = 0; round != 10; ++round) {
        quarter_round(working[0], working[4], working[8], working[12]);
        quarter_round(working[1], working[5], working[9], working[13]);
        quarter_round(working[2], working[6], working[10], working[14]);
        quarter_round(working[3], working[7], working[11], working[15]);
        quarter_round(working[0], working[5], working[10], working[15]);
        quarter_round(working[1], working[6], working[11], working[12]);
        quarter_round(working[2], working[7], working[8], working[13]);
        quarter_round(working[3], working[4], working[9], working[14]);
    }
    auto output = std::array<std::byte, 64>{};
    for (auto index = std::size_t{}; index != working.size(); ++index) {
        store_u32(output.data() + index * 4, working[index] + state[index]);
    }
    return output;
}

auto chacha_xor(std::span<const std::byte, 32> key_bytes,
                std::span<const std::byte, 12> nonce,
                std::span<const std::byte> input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(input.size());
    auto counter = std::uint32_t{1};
    for (auto offset = std::size_t{}; offset < input.size(); offset += 64) {
        auto block = chacha_block(key_bytes, counter++, nonce);
        const auto count = std::min<std::size_t>(block.size(), input.size() - offset);
        for (auto index = std::size_t{}; index != count; ++index) {
            output[offset + index] = input[offset + index] ^ block[index];
        }
        crypto_wipe(block.data(), block.size());
    }
    return output;
}

auto authentication_data(std::span<const std::byte> additional_data,
                         std::span<const std::byte> ciphertext) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    output.insert(output.end(), additional_data.begin(), additional_data.end());
    output.insert(output.end(), (16 - additional_data.size() % 16) % 16, std::byte{});
    output.insert(output.end(), ciphertext.begin(), ciphertext.end());
    output.insert(output.end(), (16 - ciphertext.size() % 16) % 16, std::byte{});
    put_u64(output, additional_data.size());
    put_u64(output, ciphertext.size());
    return output;
}

auto constant_time_equal(std::span<const std::byte> left, std::span<const std::byte> right)
    -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    auto difference = std::uint8_t{};
    for (auto index = std::size_t{}; index != left.size(); ++index) {
        difference |= std::to_integer<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

} // namespace

auto custody_error(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto reference(const wrap_context& context) -> std::string {
    return context.key_id + "@" + std::to_string(context.version);
}

auto as_bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto input = as_bytes(value);
    return {input.begin(), input.end()};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto base64_encode(std::span<const std::byte> input, bool url) -> std::string {
    const auto alphabet = url ? base64_url_alphabet : base64_alphabet;
    auto output = std::string{};
    output.reserve((input.size() * 4 + 2) / 3);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 3) {
        const auto remaining = input.size() - offset;
        const auto first = std::to_integer<std::uint32_t>(input[offset]);
        const auto second = remaining > 1 ? std::to_integer<std::uint32_t>(input[offset + 1]) : 0;
        const auto third = remaining > 2 ? std::to_integer<std::uint32_t>(input[offset + 2]) : 0;
        const auto group = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(group >> 18U) & 63U]);
        output.push_back(alphabet[(group >> 12U) & 63U]);
        if (remaining > 1) {
            output.push_back(alphabet[(group >> 6U) & 63U]);
        }
        if (remaining > 2) {
            output.push_back(alphabet[group & 63U]);
        }
    }
    return output;
}

auto base64_decode(std::string_view input, bool url, bool padded)
    -> result<std::vector<std::byte>> {
    if (!padded && input.find('=') != std::string_view::npos) {
        return std::unexpected(
            custody_error(errc::corrupt, "non-canonical base64 padding", "custody data"));
    }
    while (padded && input.ends_with('=')) {
        input.remove_suffix(1);
    }
    if (input.size() % 4 == 1) {
        return std::unexpected(
            custody_error(errc::corrupt, "invalid base64 length", "custody data"));
    }
    auto output = std::vector<std::byte>{};
    output.reserve(input.size() * 3 / 4);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 4) {
        const auto count = std::min<std::size_t>(4, input.size() - offset);
        auto group = std::uint32_t{};
        for (auto index = std::size_t{}; index != 4; ++index) {
            const auto digit = index < count ? base64_value(input[offset + index], url) : 0;
            if (digit < 0) {
                return std::unexpected(
                    custody_error(errc::corrupt, "invalid base64 character", "custody data"));
            }
            group = (group << 6U) | static_cast<std::uint32_t>(digit);
        }
        output.push_back(std::byte{static_cast<unsigned char>(group >> 16U)});
        if (count > 2) {
            output.push_back(std::byte{static_cast<unsigned char>(group >> 8U)});
        }
        if (count > 3) {
            output.push_back(std::byte{static_cast<unsigned char>(group)});
        }
    }
    if (base64_encode(output, url) != input) {
        return std::unexpected(
            custody_error(errc::corrupt, "non-canonical base64 encoding", "custody data"));
    }
    return output;
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto value : input) {
        const auto octet = std::to_integer<unsigned char>(value);
        output.push_back(digits[octet >> 4U]);
        output.push_back(digits[octet & 15U]);
    }
    return output;
}

auto sha256(std::span<const std::byte> input) -> sha256_digest {
    auto context = SHA256_CTX{};
    auto output = sha256_digest{};
    sha256_init(&context);
    sha256_update(&context, reinterpret_cast<const BYTE*>(input.data()), input.size());
    sha256_final(&context, reinterpret_cast<BYTE*>(output.data()));
    return output;
}

auto hmac_sha256(std::span<const std::byte> key_bytes, std::span<const std::byte> input)
    -> sha256_digest {
    auto normalized = std::array<std::byte, 64>{};
    if (key_bytes.size() > normalized.size()) {
        const auto digest = sha256(key_bytes);
        std::ranges::copy(digest, normalized.begin());
    } else {
        std::ranges::copy(key_bytes, normalized.begin());
    }
    auto inner_key = normalized;
    auto outer_key = normalized;
    for (auto index = std::size_t{}; index != normalized.size(); ++index) {
        inner_key[index] ^= std::byte{0x36};
        outer_key[index] ^= std::byte{0x5c};
    }
    auto inner_input = std::vector<std::byte>{inner_key.begin(), inner_key.end()};
    inner_input.insert(inner_input.end(), input.begin(), input.end());
    auto inner = sha256(inner_input);
    crypto_wipe(inner_input.data(), inner_input.size());
    auto outer_input = std::vector<std::byte>{outer_key.begin(), outer_key.end()};
    outer_input.insert(outer_input.end(), inner.begin(), inner.end());
    const auto output = sha256(outer_input);
    crypto_wipe(outer_input.data(), outer_input.size());
    crypto_wipe(inner.data(), inner.size());
    crypto_wipe(normalized.data(), normalized.size());
    crypto_wipe(inner_key.data(), inner_key.size());
    crypto_wipe(outer_key.data(), outer_key.size());
    return output;
}

auto hkdf_sha256(std::span<const std::byte> input,
                 std::span<const std::byte> salt,
                 std::span<const std::byte> info) -> sha256_digest {
    auto extracted = hmac_sha256(salt, input);
    auto expanded_input = std::vector<std::byte>{info.begin(), info.end()};
    expanded_input.push_back(std::byte{1});
    auto output = hmac_sha256(extracted, expanded_input);
    crypto_wipe(extracted.data(), extracted.size());
    crypto_wipe(expanded_input.data(), expanded_input.size());
    return output;
}

auto chacha20_poly1305_seal(std::span<const std::byte, 32> key_bytes,
                            std::span<const std::byte, 12> nonce,
                            std::span<const std::byte> additional_data,
                            std::span<const std::byte> plaintext) -> std::vector<std::byte> {
    auto first = chacha_block(key_bytes, 0, nonce);
    auto ciphertext = chacha_xor(key_bytes, nonce, plaintext);
    const auto mac_input = authentication_data(additional_data, ciphertext);
    auto tag = std::array<std::byte, 16>{};
    crypto_poly1305(reinterpret_cast<std::uint8_t*>(tag.data()),
                    reinterpret_cast<const std::uint8_t*>(mac_input.data()),
                    mac_input.size(),
                    reinterpret_cast<const std::uint8_t*>(first.data()));
    crypto_wipe(first.data(), first.size());
    ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
    return ciphertext;
}

auto chacha20_poly1305_open(std::span<const std::byte, 32> key_bytes,
                            std::span<const std::byte, 12> nonce,
                            std::span<const std::byte> additional_data,
                            std::span<const std::byte> boxed) -> result<std::vector<std::byte>> {
    if (boxed.size() < 16) {
        return std::unexpected(
            custody_error(errc::corrupt, "truncated ChaCha20-Poly1305 value", "custody data"));
    }
    const auto ciphertext = boxed.first(boxed.size() - 16);
    const auto expected = boxed.last(16);
    auto first = chacha_block(key_bytes, 0, nonce);
    const auto mac_input = authentication_data(additional_data, ciphertext);
    auto actual = std::array<std::byte, 16>{};
    crypto_poly1305(reinterpret_cast<std::uint8_t*>(actual.data()),
                    reinterpret_cast<const std::uint8_t*>(mac_input.data()),
                    mac_input.size(),
                    reinterpret_cast<const std::uint8_t*>(first.data()));
    crypto_wipe(first.data(), first.size());
    if (!constant_time_equal(actual, expected)) {
        return std::unexpected(custody_error(
            errc::unauthorized, "ChaCha20-Poly1305 authentication failed", "custody data"));
    }
    return chacha_xor(key_bytes, nonce, ciphertext);
}

auto json(std::initializer_list<std::pair<std::string, formats::json::value>> fields)
    -> result<std::string> {
    return formats::json::write(formats::json::value{formats::json::value::object{fields}});
}

auto json_field(const formats::json::value& value, std::string_view name)
    -> const formats::json::value* {
    const auto* fields = std::get_if<formats::json::value::object>(&value.data);
    if (!fields) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(*fields, name, &std::pair<std::string, formats::json::value>::first);
    return found == fields->end() ? nullptr : &found->second;
}

auto json_string_field(const formats::json::value& value, std::string_view name)
    -> result<std::string> {
    const auto* field = json_field(value, name);
    const auto* text_value = field ? std::get_if<std::string>(&field->data) : nullptr;
    if (!text_value) {
        return std::unexpected(custody_error(
            errc::corrupt, "custody response string field is missing", std::string{name}));
    }
    return *text_value;
}

auto require_success(http_response response, const wrap_context& context) -> result<http_response> {
    if (response.status >= 200 && response.status < 300) {
        return response;
    }
    const auto code =
        response.status == 401 || response.status == 403 ? errc::unauthorized : errc::unavailable;
    return std::unexpected(
        custody_error(code,
                      "custody endpoint returned HTTP " + std::to_string(response.status),
                      reference(context)));
}

auto environment(std::string_view name) -> result<std::string> {
    if (const auto* value = std::getenv(std::string{name}.c_str()); value && *value) {
        return std::string{value};
    }
    return std::unexpected(
        custody_error(errc::unauthorized, "custody credential is unavailable", std::string{name}));
}

auto optional_environment(std::string_view name) -> std::string {
    if (const auto* value = std::getenv(std::string{name}.c_str())) {
        return value;
    }
    return {};
}

} // namespace liquid::custody_detail
