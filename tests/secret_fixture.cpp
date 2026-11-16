// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cstddef>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/crypto.hpp"
#include "liquid/secret.hpp"
#include "secret/secret.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

template <std::size_t Size> auto fixed(std::string_view value) -> std::array<std::byte, Size> {
    auto output = std::array<std::byte, Size>{};
    std::ranges::copy(bytes(value), output.begin());
    return output;
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto value : input) {
        const auto byte = std::to_integer<unsigned char>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

} // namespace

int main() {
    const auto recipient_private = fixed<32>("0123456789abcdef0123456789abcdef");
    const auto ephemeral_private = fixed<32>("fedcba9876543210fedcba9876543210");
    const auto nonce = fixed<24>("nonce-for-liquid-fixture");
    const auto plaintext = bytes("correct horse battery staple\n");
    auto recipient_public = liquid::crypto_detail::x25519_public_key(recipient_private);
    auto ephemeral_public = liquid::crypto_detail::x25519_public_key(ephemeral_private);
    if (!recipient_public || !ephemeral_public) {
        return 1;
    }
    auto random_input = std::vector<std::byte>{ephemeral_private.begin(), ephemeral_private.end()};
    random_input.insert(random_input.end(), nonce.begin(), nonce.end());
    auto offset = std::size_t{};
    auto sealed = liquid::secret_detail::seal_secret_with_random(
        liquid::sealing_public_key{.kid = "lake-key-2026-09", .key = *recipient_public},
        liquid::seal_scope{.cluster = "lake-prod", .namespace_ = "payments", .name = "database"},
        plaintext,
        [&](std::span<std::byte> output) -> liquid::result<void> {
            if (random_input.size() - offset < output.size()) {
                return std::unexpected(liquid::error{
                    liquid::errc::internal, "fixture byte source exhausted", "fixture"});
            }
            std::ranges::copy(
                std::span<const std::byte>{random_input}.subspan(offset, output.size()),
                output.begin());
            offset += output.size();
            return {};
        });
    if (!sealed) {
        return 1;
    }
    auto record = liquid::encode_sealed_secret(*sealed);
    if (!record) {
        return 1;
    }
    std::println("recipient-private.hex {}", hex(recipient_private));
    std::println("recipient-public.hex {}", hex(*recipient_public));
    std::println("ephemeral-private.hex {}", hex(ephemeral_private));
    std::println("ephemeral-public.hex {}", hex(*ephemeral_public));
    std::println("nonce.hex {}", hex(nonce));
    std::println("plaintext.hex {}", hex(plaintext));
    std::println("record.cbor.hex {}", hex(*record));
}
