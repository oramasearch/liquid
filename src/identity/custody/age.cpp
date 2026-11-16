// SPDX-License-Identifier: MIT

#include "identity/custody/age.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "crypto/crypto.hpp"
#include "identity/custody/common.hpp"
#include "identity/custody/providers.hpp"
#include "monocypher.h"

namespace liquid::custody_detail {
namespace {

constexpr auto age_version = std::string_view{"age-encryption.org/v1\n"};
constexpr auto x25519_info = std::string_view{"age-encryption.org/v1/X25519"};
constexpr auto header_info = std::string_view{"header"};
constexpr auto payload_info = std::string_view{"payload"};

struct stanza {
    std::array<std::byte, 32> share{};
    std::vector<std::byte> body;
};

struct parsed_age {
    std::vector<stanza> stanzas;
    std::string authenticated_header;
    std::array<std::byte, 32> mac{};
    std::span<const std::byte> payload;
};

auto split(std::string_view input) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    while (!input.empty()) {
        const auto separator = input.find_first_of(",\n");
        auto value = input.substr(0, separator);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        if (!value.empty()) {
            output.emplace_back(value);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        input.remove_prefix(separator + 1);
    }
    return output;
}

auto bech32_polymod(std::span<const std::uint8_t> values) -> std::uint32_t {
    constexpr auto generator = std::array<std::uint32_t, 5>{
        0x3b6a57b2U, 0x26508e6dU, 0x1ea119faU, 0x3d4233ddU, 0x2a1462b3U};
    auto checksum = std::uint32_t{1};
    for (const auto value : values) {
        const auto top = checksum >> 25U;
        checksum = ((checksum & 0x1ffffffU) << 5U) ^ value;
        for (auto index = std::size_t{}; index != generator.size(); ++index) {
            if ((top >> index) & 1U) {
                checksum ^= generator[index];
            }
        }
    }
    return checksum;
}

auto bech32_decode(std::string_view encoded, std::string_view expected_hrp)
    -> result<std::array<std::byte, 32>> {
    constexpr auto alphabet = std::string_view{"qpzry9x8gf2tvdw0s3jn54khce6mua7l"};
    auto has_lower = false;
    auto has_upper = false;
    auto normalized = std::string{encoded};
    for (auto& character : normalized) {
        has_lower = has_lower || std::islower(static_cast<unsigned char>(character));
        has_upper = has_upper || std::isupper(static_cast<unsigned char>(character));
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const auto separator = normalized.rfind('1');
    if ((has_lower && has_upper) || separator == std::string::npos || separator == 0 ||
        normalized.size() - separator < 7 || normalized.substr(0, separator) != expected_hrp) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid age Bech32 key", std::string{encoded}));
    }
    auto values = std::vector<std::uint8_t>{};
    values.reserve(separator * 2 + normalized.size() - separator);
    for (const auto character : normalized.substr(0, separator)) {
        values.push_back(static_cast<std::uint8_t>(character >> 5U));
    }
    values.push_back(0);
    for (const auto character : normalized.substr(0, separator)) {
        values.push_back(static_cast<std::uint8_t>(character & 31U));
    }
    auto data = std::vector<std::uint8_t>{};
    for (const auto character : normalized.substr(separator + 1)) {
        const auto found = alphabet.find(character);
        if (found == std::string_view::npos) {
            return std::unexpected(custody_error(
                errc::invalid_argument, "invalid age Bech32 character", std::string{encoded}));
        }
        data.push_back(static_cast<std::uint8_t>(found));
        values.push_back(static_cast<std::uint8_t>(found));
    }
    if (bech32_polymod(values) != 1) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "invalid age Bech32 checksum", std::string{encoded}));
    }
    data.resize(data.size() - 6);
    auto output_bytes = std::vector<std::byte>{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto value : data) {
        accumulator = (accumulator << 5U) | value;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            output_bytes.push_back(
                std::byte{static_cast<unsigned char>((accumulator >> bits) & 0xffU)});
        }
    }
    if (bits >= 5 || ((accumulator << (8U - bits)) & 0xffU) != 0 ||
        output_bytes.size() != key{}.size()) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "invalid age Bech32 key length", std::string{encoded}));
    }
    auto output = std::array<std::byte, 32>{};
    std::ranges::copy(output_bytes, output.begin());
    return output;
}

auto parse_age(std::span<const std::byte> input) -> result<parsed_age> {
    const auto wire = text(input);
    if (!wire.starts_with(age_version)) {
        return std::unexpected(
            custody_error(errc::corrupt, "unsupported age header", "age custody"));
    }
    auto output = parsed_age{};
    auto position = age_version.size();
    while (position < wire.size() && !std::string_view{wire}.substr(position).starts_with("--- ")) {
        const auto argument_end = wire.find('\n', position);
        if (argument_end == std::string::npos) {
            return std::unexpected(
                custody_error(errc::corrupt, "truncated age stanza", "age custody"));
        }
        const auto arguments = std::string_view{wire}.substr(position, argument_end - position);
        constexpr auto prefix = std::string_view{"-> X25519 "};
        if (!arguments.starts_with(prefix) ||
            arguments.substr(prefix.size()).find(' ') != std::string_view::npos) {
            return std::unexpected(
                custody_error(errc::corrupt, "unsupported age recipient stanza", "age custody"));
        }
        auto share = base64_decode(arguments.substr(prefix.size()));
        if (!share || share->size() != 32) {
            return std::unexpected(
                custody_error(errc::corrupt, "invalid age X25519 share", "age custody"));
        }
        auto current = stanza{};
        std::ranges::copy(*share, current.share.begin());
        position = argument_end + 1;
        for (;;) {
            const auto body_end = wire.find('\n', position);
            if (body_end == std::string::npos) {
                return std::unexpected(
                    custody_error(errc::corrupt, "truncated age stanza body", "age custody"));
            }
            const auto line = std::string_view{wire}.substr(position, body_end - position);
            auto body = base64_decode(line);
            if (!body) {
                return std::unexpected(body.error());
            }
            current.body.insert(current.body.end(), body->begin(), body->end());
            position = body_end + 1;
            if (line.size() < 64) {
                break;
            }
        }
        if (current.body.size() != 32) {
            return std::unexpected(
                custody_error(errc::corrupt, "invalid age X25519 body", "age custody"));
        }
        output.stanzas.push_back(std::move(current));
    }
    if (output.stanzas.empty() || position >= wire.size()) {
        return std::unexpected(
            custody_error(errc::corrupt, "age header has no recipients", "age custody"));
    }
    const auto mac_end = wire.find('\n', position);
    if (mac_end == std::string::npos) {
        return std::unexpected(
            custody_error(errc::corrupt, "truncated age header MAC", "age custody"));
    }
    const auto mac_line = std::string_view{wire}.substr(position, mac_end - position);
    auto mac = base64_decode(mac_line.substr(4));
    if (!mac_line.starts_with("--- ") || !mac || mac->size() != output.mac.size()) {
        return std::unexpected(
            custody_error(errc::corrupt, "invalid age header MAC", "age custody"));
    }
    output.authenticated_header = wire.substr(0, position + 3);
    std::ranges::copy(*mac, output.mac.begin());
    output.payload = input.subspan(mac_end + 1);
    return output;
}

auto unwrap_file_key(const parsed_age& parsed, std::span<const std::string_view> identities)
    -> result<std::array<std::byte, 16>> {
    const auto zero_nonce = std::array<std::byte, 12>{};
    for (const auto identity_text : identities) {
        auto secret = bech32_decode(identity_text, "age-secret-key-");
        if (!secret) {
            continue;
        }
        auto recipient = std::array<std::byte, 32>{};
        crypto_x25519_public_key(reinterpret_cast<std::uint8_t*>(recipient.data()),
                                 reinterpret_cast<const std::uint8_t*>(secret->data()));
        for (const auto& value : parsed.stanzas) {
            auto shared = std::array<std::byte, 32>{};
            crypto_x25519(reinterpret_cast<std::uint8_t*>(shared.data()),
                          reinterpret_cast<const std::uint8_t*>(secret->data()),
                          reinterpret_cast<const std::uint8_t*>(value.share.data()));
            if (std::ranges::all_of(shared, [](std::byte octet) { return octet == std::byte{}; })) {
                continue;
            }
            auto salt = std::vector<std::byte>{value.share.begin(), value.share.end()};
            salt.insert(salt.end(), recipient.begin(), recipient.end());
            auto wrapping = hkdf_sha256(shared, salt, as_bytes(x25519_info));
            auto opened = chacha20_poly1305_open(wrapping, zero_nonce, {}, value.body);
            crypto_detail::wipe(shared);
            crypto_detail::wipe(wrapping);
            if (!opened || opened->size() != 16) {
                continue;
            }
            auto file_key = std::array<std::byte, 16>{};
            std::ranges::copy(*opened, file_key.begin());
            crypto_detail::wipe(*opened);
            crypto_detail::wipe(*secret);
            return file_key;
        }
        crypto_detail::wipe(*secret);
    }
    return std::unexpected(
        custody_error(errc::unauthorized, "no age identity matched", "age custody"));
}

auto decrypt_payload(std::span<const std::byte> payload, std::span<const std::byte, 16> file_key)
    -> result<std::vector<std::byte>> {
    if (payload.size() < 32) {
        return std::unexpected(
            custody_error(errc::corrupt, "truncated age payload", "age custody"));
    }
    const auto nonce = payload.first<16>();
    auto key_bytes = hkdf_sha256(file_key, nonce, as_bytes(payload_info));
    payload = payload.subspan(16);
    auto output = std::vector<std::byte>{};
    auto counter = std::uint64_t{};
    while (!payload.empty()) {
        constexpr auto full_chunk = std::size_t{64} * 1024 + 16;
        const auto final = payload.size() <= full_chunk;
        const auto count = final ? payload.size() : full_chunk;
        auto chunk_nonce = std::array<std::byte, 12>{};
        auto value = counter++;
        for (auto index = 0U; index != 8U; ++index) {
            chunk_nonce[10 - index] = std::byte{static_cast<unsigned char>(value)};
            value >>= 8U;
        }
        chunk_nonce.back() = final ? std::byte{1} : std::byte{};
        auto opened = chacha20_poly1305_open(key_bytes, chunk_nonce, {}, payload.first(count));
        if (!opened) {
            crypto_detail::wipe(key_bytes);
            return std::unexpected(opened.error());
        }
        output.insert(output.end(), opened->begin(), opened->end());
        crypto_detail::wipe(*opened);
        payload = payload.subspan(count);
    }
    crypto_detail::wipe(key_bytes);
    return output;
}

auto age_encrypt(std::span<const std::byte> plaintext,
                 std::span<const std::array<std::byte, 32>> recipients)
    -> result<std::vector<std::byte>> {
    auto file_key = std::array<std::byte, 16>{};
    auto filled = crypto_detail::random_bytes(file_key);
    if (!filled) {
        return std::unexpected(filled.error());
    }
    auto header = std::string{age_version};
    const auto zero_nonce = std::array<std::byte, 12>{};
    for (const auto& recipient : recipients) {
        auto secret = std::array<std::byte, 32>{};
        filled = crypto_detail::random_bytes(secret);
        if (!filled) {
            crypto_detail::wipe(file_key);
            return std::unexpected(filled.error());
        }
        auto share = std::array<std::byte, 32>{};
        auto shared = std::array<std::byte, 32>{};
        crypto_x25519_public_key(reinterpret_cast<std::uint8_t*>(share.data()),
                                 reinterpret_cast<const std::uint8_t*>(secret.data()));
        crypto_x25519(reinterpret_cast<std::uint8_t*>(shared.data()),
                      reinterpret_cast<const std::uint8_t*>(secret.data()),
                      reinterpret_cast<const std::uint8_t*>(recipient.data()));
        if (std::ranges::all_of(shared, [](std::byte octet) { return octet == std::byte{}; })) {
            crypto_detail::wipe(secret);
            crypto_detail::wipe(file_key);
            return std::unexpected(
                custody_error(errc::invalid_argument, "invalid age recipient", "age custody"));
        }
        auto salt = std::vector<std::byte>{share.begin(), share.end()};
        salt.insert(salt.end(), recipient.begin(), recipient.end());
        auto wrapping = hkdf_sha256(shared, salt, as_bytes(x25519_info));
        const auto body = chacha20_poly1305_seal(wrapping, zero_nonce, {}, file_key);
        header += "-> X25519 " + base64_encode(share) + "\n" + base64_encode(body) + "\n";
        crypto_detail::wipe(secret);
        crypto_detail::wipe(shared);
        crypto_detail::wipe(wrapping);
    }
    header += "---";
    auto mac_key = hkdf_sha256(file_key, {}, as_bytes(header_info));
    const auto mac = hmac_sha256(mac_key, as_bytes(header));
    crypto_detail::wipe(mac_key);
    header += " " + base64_encode(mac) + "\n";
    auto output = bytes(header);
    auto nonce = std::array<std::byte, 16>{};
    filled = crypto_detail::random_bytes(nonce);
    if (!filled) {
        crypto_detail::wipe(file_key);
        return std::unexpected(filled.error());
    }
    auto payload_key = hkdf_sha256(file_key, nonce, as_bytes(payload_info));
    auto chunk_nonce = std::array<std::byte, 12>{};
    chunk_nonce.back() = std::byte{1};
    const auto payload = chacha20_poly1305_seal(payload_key, chunk_nonce, {}, plaintext);
    crypto_detail::wipe(payload_key);
    output.insert(output.end(), nonce.begin(), nonce.end());
    output.insert(output.end(), payload.begin(), payload.end());
    crypto_detail::wipe(file_key);
    return output;
}

class age_provider final : public key_provider {
public:
    age_provider(std::string recipients, std::vector<std::array<std::byte, 32>> decoded)
        : recipients_(std::move(recipients)), decoded_(std::move(decoded)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context)
        -> runtime::task<std::vector<std::byte>> override {
        co_return age_encrypt(data_key, decoded_);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        const auto values = split(optional_environment("LIQUID_AGE_IDENTITIES"));
        auto identities = std::vector<std::string_view>{};
        identities.reserve(values.size());
        for (const auto& value : values) {
            identities.push_back(value);
        }
        auto opened = age_decrypt(wrapped, identities);
        if (!opened || opened->size() != key{}.size()) {
            if (opened) {
                crypto_detail::wipe(*opened);
            }
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap age data key", reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*opened, output.begin());
        crypto_detail::wipe(*opened);
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "age";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(recipients_);
    }

private:
    std::string recipients_;
    std::vector<std::array<std::byte, 32>> decoded_;
};

} // namespace

auto age_decrypt(std::span<const std::byte> input, std::span<const std::string_view> identities)
    -> result<std::vector<std::byte>> {
    auto parsed = parse_age(input);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto file_key = unwrap_file_key(*parsed, identities);
    if (!file_key) {
        return std::unexpected(file_key.error());
    }
    auto mac_key = hkdf_sha256(*file_key, {}, as_bytes(header_info));
    const auto actual = hmac_sha256(mac_key, as_bytes(parsed->authenticated_header));
    crypto_detail::wipe(mac_key);
    if (actual != parsed->mac) {
        crypto_detail::wipe(*file_key);
        return std::unexpected(
            custody_error(errc::unauthorized, "age header authentication failed", "age custody"));
    }
    auto output = decrypt_payload(parsed->payload, *file_key);
    crypto_detail::wipe(*file_key);
    return output;
}

auto make_age_provider(std::string recipients) -> result<std::shared_ptr<key_provider>> {
    const auto values = split(recipients);
    if (values.empty()) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "age recipient list is empty", "age custody"));
    }
    auto decoded = std::vector<std::array<std::byte, 32>>{};
    decoded.reserve(values.size());
    for (const auto& value : values) {
        auto recipient = bech32_decode(value, "age");
        if (!recipient) {
            return std::unexpected(recipient.error());
        }
        decoded.push_back(*recipient);
    }
    return std::make_shared<age_provider>(std::move(recipients), std::move(decoded));
}

} // namespace liquid::custody_detail
