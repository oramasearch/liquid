// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "encoding/cbor.hpp"
#include "identity/custody/tpm.hpp"
#include "identity/keys/backend.hpp"
#include "identity/taap.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/release.hpp"
#include "liquid/runtime.hpp"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "objects/records.hpp"
#include "temporary_directory.hpp"

namespace liquid_cli {
auto identity(int argc, char** argv) -> int;
}

namespace {

namespace cbor = liquid::encoding::cbor;

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* values = std::get_if<cbor::value::map>(&source.data);
    if (!values) {
        return nullptr;
    }
    for (const auto& [key, value] : *values) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto field(const cbor::value& source, std::int64_t name) -> const cbor::value* {
    const auto* values = std::get_if<cbor::value::map>(&source.data);
    if (!values) {
        return nullptr;
    }
    for (const auto& [key, value] : *values) {
        if (const auto* signed_key = std::get_if<std::int64_t>(&key.data);
            signed_key && *signed_key == name) {
            return &value;
        }
        if (const auto* unsigned_key = std::get_if<std::uint64_t>(&key.data);
            unsigned_key && name >= 0 && *unsigned_key == static_cast<std::uint64_t>(name)) {
            return &value;
        }
    }
    return nullptr;
}

auto byte_string(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

// SHA-256 computed by OpenSSL, so the vectors below never depend on liquid's own digest code.
auto openssl_sha256(std::span<const std::byte> input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(32);
    auto size = 0U;
    EXPECT_EQ(EVP_Digest(input.data(),
                         input.size(),
                         reinterpret_cast<unsigned char*>(output.data()),
                         &size,
                         EVP_sha256(),
                         nullptr),
              1);
    EXPECT_EQ(size, 32U);
    return output;
}

// WebAuthn Level 2 §6.1 authenticator data for an assertion: rpIdHash (32) || flags (1, UP) ||
// signCount (4, big-endian). No attested credential data and no extensions.
auto ctap2_assertion_authenticator_data(std::string_view rp_id,
                                        std::uint32_t sign_count,
                                        std::byte flags = std::byte{0x01})
    -> std::vector<std::byte> {
    auto output = openssl_sha256(std::as_bytes(std::span{rp_id}));
    output.push_back(flags);
    output.push_back(std::byte{static_cast<std::uint8_t>(sign_count >> 24)});
    output.push_back(std::byte{static_cast<std::uint8_t>(sign_count >> 16)});
    output.push_back(std::byte{static_cast<std::uint8_t>(sign_count >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(sign_count)});
    return output;
}

// The rpId (CTAP2 getAssertion key 1) the client asked for, as a real authenticator would hash it.
auto assertion_authenticator_data(const cbor::value& request, std::uint32_t sign_count)
    -> std::vector<std::byte> {
    const auto* rp = field(request, 1);
    const auto* rp_id = rp ? std::get_if<std::string>(&rp->data) : nullptr;
    if (!rp_id || rp_id->empty()) {
        return {};
    }
    return ctap2_assertion_authenticator_data(*rp_id, sign_count);
}

class fido_double final : public liquid::identity_detail::fido2_transport {
public:
    fido_double() {
        auto seed = std::array<std::uint8_t, 32>{};
        for (std::size_t index = 0; index < seed.size(); ++index) {
            seed[index] = static_cast<std::uint8_t>(index + 1);
        }
        crypto_ed25519_key_pair(secret_.data(), public_key_.data(), seed.data());
    }

    auto transact(std::uint8_t command, std::span<const std::byte> payload)
        -> liquid::result<std::vector<std::byte>> override {
        auto decoded = cbor::decode(payload);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (command == 0x01) {
            auto algorithms = field(*decoded, 4);
            auto extensions = field(*decoded, 6);
            if (!algorithms || !extensions) {
                return std::unexpected(liquid::error{
                    liquid::errc::corrupt, "invalid makeCredential command", "FIDO2 double"});
            }
            auto cose = cbor::encode(cbor::value{cbor::value::map{
                {cbor::value{std::uint64_t{1}}, cbor::value{std::uint64_t{1}}},
                {cbor::value{std::uint64_t{3}}, cbor::value{std::int64_t{-8}}},
                {cbor::value{std::int64_t{-1}}, cbor::value{std::uint64_t{6}}},
                {cbor::value{std::int64_t{-2}}, byte_string(std::as_bytes(std::span{public_key_}))},
            }});
            if (!cose) {
                return std::unexpected(cose.error());
            }
            auto authenticator_data = std::vector<std::byte>(53, std::byte{0});
            authenticator_data[32] = std::byte{0x41};
            authenticator_data.push_back(std::byte{0x00});
            authenticator_data.push_back(std::byte{static_cast<std::uint8_t>(credential_.size())});
            authenticator_data.insert(
                authenticator_data.end(), credential_.begin(), credential_.end());
            authenticator_data.insert(authenticator_data.end(), cose->begin(), cose->end());
            return cbor::encode(cbor::value{cbor::value::map{
                {cbor::value{std::uint64_t{1}}, "packed"},
                {cbor::value{std::uint64_t{2}}, byte_string(authenticator_data)},
                {cbor::value{std::uint64_t{3}}, cbor::value{cbor::value::map{}}},
            }});
        }
        if (command != 0x02) {
            return std::unexpected(liquid::error{
                liquid::errc::unsupported, "unsupported CTAP2 command", "FIDO2 double"});
        }
        const auto* allow = field(*decoded, 3);
        const auto* allowed = allow ? std::get_if<cbor::value::array>(&allow->data) : nullptr;
        const auto* credential =
            allowed && !allowed->empty() ? field(allowed->front(), "id") : nullptr;
        const auto* client_data = field(*decoded, 2);
        const auto* credential_bytes =
            credential ? std::get_if<cbor::value::bytes>(&credential->data) : nullptr;
        const auto* client_bytes =
            client_data ? std::get_if<cbor::value::bytes>(&client_data->data) : nullptr;
        if (!credential_bytes || *credential_bytes != credential_ || !client_bytes ||
            client_bytes->size() != 32) {
            return std::unexpected(liquid::error{
                liquid::errc::unauthorized, "unknown FIDO2 credential", "FIDO2 double"});
        }
        // CTAP 2.2 authenticatorGetAssertion: EdDSA signs authenticatorData || clientDataHash
        // directly, with the client's 32 bytes exactly as received.
        auto authenticator_data = assertion_authenticator_data(*decoded, ++sign_count_);
        if (authenticator_data.empty()) {
            return std::unexpected(
                liquid::error{liquid::errc::corrupt, "missing rpId", "FIDO2 double"});
        }
        auto assertion = authenticator_data;
        assertion.insert(assertion.end(), client_bytes->begin(), client_bytes->end());
        auto signature = std::vector<std::byte>(64);
        crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()),
                            secret_.data(),
                            reinterpret_cast<const std::uint8_t*>(assertion.data()),
                            assertion.size());
        return cbor::encode(cbor::value{cbor::value::map{
            {cbor::value{std::uint64_t{2}}, byte_string(authenticator_data)},
            {cbor::value{std::uint64_t{3}}, byte_string(signature)},
        }});
    }

    auto private_key() const -> std::span<const std::byte> {
        return std::as_bytes(std::span{secret_});
    }

private:
    std::array<std::uint8_t, 64> secret_{};
    std::array<std::uint8_t, 32> public_key_{};
    std::vector<std::byte> credential_{std::byte{0x81}, std::byte{0x02}, std::byte{0x03}};
    std::uint32_t sign_count_{};
};

class p256_key {
public:
    p256_key() {
        auto context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        EXPECT_NE(context, nullptr);
        EXPECT_GT(EVP_PKEY_keygen_init(context), 0);
        EXPECT_GT(EVP_PKEY_CTX_set_group_name(context, "P-256"), 0);
        EXPECT_GT(EVP_PKEY_keygen(context, &key_), 0);
        EVP_PKEY_CTX_free(context);
    }

    ~p256_key() {
        EVP_PKEY_free(key_);
    }

    auto public_key() const -> std::vector<std::byte> {
        auto size = std::size_t{};
        EXPECT_GT(EVP_PKEY_get_octet_string_param(key_, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &size),
                  0);
        auto encoded = std::vector<unsigned char>(size);
        EXPECT_GT(EVP_PKEY_get_octet_string_param(
                      key_, OSSL_PKEY_PARAM_PUB_KEY, encoded.data(), encoded.size(), &size),
                  0);
        EXPECT_EQ(size, 65U);
        return {reinterpret_cast<const std::byte*>(encoded.data() + 1),
                reinterpret_cast<const std::byte*>(encoded.data() + size)};
    }

    auto private_key() const -> std::vector<std::byte> {
        auto scalar = static_cast<BIGNUM*>(nullptr);
        EXPECT_GT(EVP_PKEY_get_bn_param(key_, OSSL_PKEY_PARAM_PRIV_KEY, &scalar), 0);
        auto output = std::vector<std::byte>(32);
        if (scalar) {
            EXPECT_EQ(BN_bn2binpad(scalar, reinterpret_cast<unsigned char*>(output.data()), 32),
                      32);
        }
        BN_clear_free(scalar);
        return output;
    }

    auto sign_digest(std::span<const std::byte> digest) const -> std::vector<std::byte> {
        auto context = EVP_PKEY_CTX_new(key_, nullptr);
        EXPECT_NE(context, nullptr);
        EXPECT_GT(EVP_PKEY_sign_init(context), 0);
        EXPECT_GT(EVP_PKEY_CTX_set_signature_md(context, EVP_sha256()), 0);
        auto size = std::size_t{};
        EXPECT_GT(EVP_PKEY_sign(context,
                                nullptr,
                                &size,
                                reinterpret_cast<const unsigned char*>(digest.data()),
                                digest.size()),
                  0);
        auto output = std::vector<std::byte>(size);
        EXPECT_GT(EVP_PKEY_sign(context,
                                reinterpret_cast<unsigned char*>(output.data()),
                                &size,
                                reinterpret_cast<const unsigned char*>(digest.data()),
                                digest.size()),
                  0);
        EVP_PKEY_CTX_free(context);
        output.resize(size);
        return output;
    }

    // ECDSA with SHA-256 over the message, hashed by OpenSSL (RFC 8152 ES256 / FIPS 186-5).
    auto sign_message(std::span<const std::byte> message) const -> std::vector<std::byte> {
        auto context = EVP_MD_CTX_new();
        EXPECT_NE(context, nullptr);
        EXPECT_GT(EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key_), 0);
        auto size = std::size_t{};
        EXPECT_GT(EVP_DigestSign(context,
                                 nullptr,
                                 &size,
                                 reinterpret_cast<const unsigned char*>(message.data()),
                                 message.size()),
                  0);
        auto output = std::vector<std::byte>(size);
        EXPECT_GT(EVP_DigestSign(context,
                                 reinterpret_cast<unsigned char*>(output.data()),
                                 &size,
                                 reinterpret_cast<const unsigned char*>(message.data()),
                                 message.size()),
                  0);
        EVP_MD_CTX_free(context);
        output.resize(size);
        return output;
    }

private:
    EVP_PKEY* key_{};
};

class fido_es256_double final : public liquid::identity_detail::fido2_transport {
public:
    auto transact(std::uint8_t command, std::span<const std::byte> payload)
        -> liquid::result<std::vector<std::byte>> override {
        auto decoded = cbor::decode(payload);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (command == 0x01) {
            const auto public_key = key_.public_key();
            auto cose = cbor::encode(cbor::value{cbor::value::map{
                {cbor::value{std::uint64_t{1}}, cbor::value{std::uint64_t{2}}},
                {cbor::value{std::uint64_t{3}}, cbor::value{std::int64_t{-7}}},
                {cbor::value{std::int64_t{-1}}, cbor::value{std::uint64_t{1}}},
                {cbor::value{std::int64_t{-2}}, byte_string(std::span{public_key}.first(32))},
                {cbor::value{std::int64_t{-3}}, byte_string(std::span{public_key}.subspan(32))},
            }});
            if (!cose) {
                return std::unexpected(cose.error());
            }
            auto authenticator_data = std::vector<std::byte>(53, std::byte{0});
            authenticator_data[32] = std::byte{0x41};
            authenticator_data.push_back(std::byte{0x00});
            authenticator_data.push_back(std::byte{static_cast<std::uint8_t>(credential_.size())});
            authenticator_data.insert(
                authenticator_data.end(), credential_.begin(), credential_.end());
            authenticator_data.insert(authenticator_data.end(), cose->begin(), cose->end());
            return cbor::encode(cbor::value{cbor::value::map{
                {cbor::value{std::uint64_t{1}}, "packed"},
                {cbor::value{std::uint64_t{2}}, byte_string(authenticator_data)},
                {cbor::value{std::uint64_t{3}}, cbor::value{cbor::value::map{}}},
            }});
        }
        if (command != 0x02) {
            return std::unexpected(liquid::error{
                liquid::errc::unsupported, "unsupported CTAP2 command", "FIDO2 ES256 double"});
        }
        const auto* allow = field(*decoded, 3);
        const auto* allowed = allow ? std::get_if<cbor::value::array>(&allow->data) : nullptr;
        const auto* credential =
            allowed && !allowed->empty() ? field(allowed->front(), "id") : nullptr;
        const auto* client_data = field(*decoded, 2);
        const auto* credential_bytes =
            credential ? std::get_if<cbor::value::bytes>(&credential->data) : nullptr;
        const auto* client_bytes =
            client_data ? std::get_if<cbor::value::bytes>(&client_data->data) : nullptr;
        if (!credential_bytes || *credential_bytes != credential_ || !client_bytes ||
            client_bytes->size() != 32) {
            return std::unexpected(liquid::error{
                liquid::errc::unauthorized, "unknown FIDO2 credential", "FIDO2 ES256 double"});
        }
        // CTAP 2.2 authenticatorGetAssertion: ES256 is ECDSA P-256 with SHA-256 over
        // authenticatorData || clientDataHash; OpenSSL does the hashing, not liquid.
        auto authenticator_data = assertion_authenticator_data(*decoded, ++sign_count_);
        if (authenticator_data.empty()) {
            return std::unexpected(
                liquid::error{liquid::errc::corrupt, "missing rpId", "FIDO2 ES256 double"});
        }
        auto assertion = authenticator_data;
        assertion.insert(assertion.end(), client_bytes->begin(), client_bytes->end());
        const auto signature = key_.sign_message(assertion);
        return cbor::encode(cbor::value{cbor::value::map{
            {cbor::value{std::uint64_t{2}}, byte_string(authenticator_data)},
            {cbor::value{std::uint64_t{3}}, byte_string(signature)},
        }});
    }

    auto private_key() const -> std::vector<std::byte> {
        return key_.private_key();
    }

private:
    p256_key key_;
    std::vector<std::byte> credential_{std::byte{0x82}, std::byte{0x04}, std::byte{0x08}};
    std::uint32_t sign_count_{};
};

auto tlv(std::initializer_list<std::byte> tag, std::span<const std::byte> value)
    -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{tag.begin(), tag.end()};
    if (value.size() < 0x80U) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value.size())});
    } else {
        output.push_back(std::byte{0x81});
        output.push_back(std::byte{static_cast<std::uint8_t>(value.size())});
    }
    output.insert(output.end(), value.begin(), value.end());
    return output;
}

class piv_double final : public liquid::identity_detail::piv_transport {
public:
    auto transmit(std::span<const std::byte> apdu)
        -> liquid::result<std::vector<std::byte>> override {
        if (apdu.size() == 15 && apdu[1] == std::byte{0xa4} && apdu[4] == std::byte{0x09} &&
            apdu.back() == std::byte{0x00}) {
            return std::vector<std::byte>{std::byte{0x90}, std::byte{0x00}};
        }
        if (apdu.size() == 11 && apdu[1] == std::byte{0x47} && apdu[3] == std::byte{0x9c} &&
            apdu.back() == std::byte{0x00}) {
            auto point = key_.public_key();
            point.insert(point.begin(), std::byte{0x04});
            auto encoded_point = tlv({std::byte{0x86}}, point);
            auto output = tlv({std::byte{0x7f}, std::byte{0x49}}, encoded_point);
            output.push_back(std::byte{0x90});
            output.push_back(std::byte{0x00});
            return output;
        }
        if (apdu.size() == 44 && apdu[1] == std::byte{0x87} && apdu[3] == std::byte{0x9c} &&
            apdu[4] == std::byte{0x26} && apdu[5] == std::byte{0x7c} &&
            apdu[6] == std::byte{0x24} && apdu[7] == std::byte{0x82} &&
            apdu[8] == std::byte{0x00} && apdu[9] == std::byte{0x81} &&
            apdu[10] == std::byte{0x20} && apdu.back() == std::byte{0x00}) {
            auto signature = key_.sign_digest(apdu.subspan(11, 32));
            auto encoded_signature = tlv({std::byte{0x82}}, signature);
            auto output = tlv({std::byte{0x7c}}, encoded_signature);
            output.push_back(std::byte{0x90});
            output.push_back(std::byte{0x00});
            return output;
        }
        return std::unexpected(
            liquid::error{liquid::errc::unauthorized, "unsupported PIV APDU", "PIV double"});
    }

    auto private_key() const -> std::vector<std::byte> {
        return key_.private_key();
    }

private:
    p256_key key_;
};

auto put_u32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 24)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 16)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

auto get_u32(std::span<const std::byte> input, std::size_t offset) -> std::uint32_t {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 3]));
}

auto put_u16(std::vector<std::byte>& output, std::uint16_t value) -> void {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

auto get_u16(std::span<const std::byte> input, std::size_t offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) << 8) |
        std::to_integer<std::uint8_t>(input[offset + 1]));
}

class tpm_double final : public liquid::custody_detail::tpm_transport {
public:
    auto submit(std::span<const std::byte> request)
        -> liquid::result<std::vector<std::byte>> override {
        if (request.size() < 10) {
            return reply(0x101, {});
        }
        const auto code = get_u32(request, 6);
        const auto body = request.subspan(10);
        if (code == 0x00000131) {
            auto output = std::vector<std::byte>{};
            put_u32(output, 0x80000000);
            put_u32(output, 0);
            return reply(0, output);
        }
        if (code == 0x00000176) {
            auto output = std::vector<std::byte>{};
            put_u32(output, 0x03000000);
            put_u16(output, 0);
            return reply(0, output);
        }
        if (code == 0x0000017f && body.size() >= 4) {
            auto policy_input = std::vector<std::byte>{body.begin() + 4, body.end()};
            policy_input.insert(policy_input.end(), pcr_state_.begin(), pcr_state_.end());
            auto policy = liquid::hash(liquid::algorithm::sha256, policy_input);
            if (!policy) {
                return std::unexpected(policy.error());
            }
            current_policy_.assign(policy->bytes.begin(), policy->bytes.end());
            return reply(0, {});
        }
        if (code == 0x00000189) {
            auto output = std::vector<std::byte>{};
            put_u16(output, static_cast<std::uint16_t>(current_policy_.size()));
            output.insert(output.end(), current_policy_.begin(), current_policy_.end());
            return reply(0, output);
        }
        if (code == 0x00000153 && body.size() > 17) {
            const auto authorization_size = get_u32(body, 4);
            auto offset = std::size_t{8 + authorization_size};
            const auto sensitive_size = get_u16(body, offset);
            offset += 2;
            const auto data_size = get_u16(body, offset + 2);
            if (sensitive_size < 36 || data_size != 32) {
                return reply(0x101, {});
            }
            sealed_ = current_policy_;
            sealed_.insert(sealed_.end(), body.begin() + offset + 4, body.begin() + offset + 36);
            auto parameters = std::vector<std::byte>{};
            put_u16(parameters, static_cast<std::uint16_t>(sealed_.size()));
            parameters.insert(parameters.end(), sealed_.begin(), sealed_.end());
            put_u16(parameters, 1);
            parameters.push_back(std::byte{1});
            auto output = std::vector<std::byte>{};
            put_u32(output, static_cast<std::uint32_t>(parameters.size()));
            output.insert(output.end(), parameters.begin(), parameters.end());
            return reply(0, output);
        }
        if (code == 0x00000157 && body.size() > 17) {
            const auto authorization_size = get_u32(body, 4);
            auto offset = std::size_t{8 + authorization_size};
            const auto private_size = get_u16(body, offset);
            offset += 2;
            if (offset + private_size > body.size()) {
                return reply(0x101, {});
            }
            loaded_.assign(body.begin() + offset, body.begin() + offset + private_size);
            auto output = std::vector<std::byte>{};
            put_u32(output, 0x80000001);
            put_u32(output, 2);
            put_u16(output, 0);
            return reply(0, output);
        }
        if (code == 0x0000015e && loaded_.size() == 64) {
            if (!std::ranges::equal(current_policy_, std::span{loaded_}.first(32))) {
                return reply(0x99d, {});
            }
            auto output = std::vector<std::byte>{};
            put_u32(output, 34);
            put_u16(output, 32);
            output.insert(output.end(), loaded_.begin() + 32, loaded_.end());
            return reply(0, output);
        }
        if (code == 0x00000165) {
            return reply(0, {});
        }
        return reply(0x101, {});
    }

    void change_pcr() {
        pcr_state_.front() ^= std::byte{1};
    }

private:
    auto reply(std::uint32_t status, std::span<const std::byte> body)
        -> liquid::result<std::vector<std::byte>> {
        auto output = std::vector<std::byte>{std::byte{0x80}, std::byte{0x02}};
        put_u32(output, static_cast<std::uint32_t>(10 + body.size()));
        put_u32(output, status);
        output.insert(output.end(), body.begin(), body.end());
        return output;
    }

    std::array<std::byte, 32> pcr_state_{};
    std::vector<std::byte> current_policy_;
    std::vector<std::byte> sealed_;
    std::vector<std::byte> loaded_;
};

#if defined(__linux__)
class swtpm_transport final : public liquid::custody_detail::tpm_transport {
public:
    explicit swtpm_transport(std::string path) : path_(std::move(path)) {}

    ~swtpm_transport() override {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    auto submit(std::span<const std::byte> request)
        -> liquid::result<std::vector<std::byte>> override {
        auto lock = std::scoped_lock{mutex_};
        auto connected = connect();
        if (!connected) {
            return std::unexpected(connected.error());
        }
        auto written = write_all(request);
        if (!written) {
            return std::unexpected(written.error());
        }
        auto header = std::array<std::byte, 10>{};
        auto read = read_all(header);
        if (!read) {
            return std::unexpected(read.error());
        }
        const auto size = get_u32(header, 2);
        if (size < header.size() || size > 4096) {
            return std::unexpected(
                liquid::error{liquid::errc::corrupt, "invalid swtpm response size", path_});
        }
        auto output = std::vector<std::byte>{header.begin(), header.end()};
        output.resize(size);
        read = read_all(std::span{output}.subspan(header.size()));
        if (!read) {
            return std::unexpected(read.error());
        }
        return output;
    }

private:
    auto connect() -> liquid::result<void> {
        if (descriptor_ >= 0) {
            return {};
        }
        auto address = sockaddr_un{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) {
            return std::unexpected(liquid::error{
                liquid::errc::invalid_argument, "swtpm socket path is too long", path_});
        }
        descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor_ < 0) {
            return io_error("cannot create swtpm socket");
        }
        std::ranges::copy(path_, address.sun_path);
        if (::connect(descriptor_,
                      reinterpret_cast<const sockaddr*>(&address),
                      static_cast<socklen_t>(sizeof(address))) != 0) {
            return io_error("cannot connect to swtpm");
        }
        return {};
    }

    auto write_all(std::span<const std::byte> input) -> liquid::result<void> {
        auto offset = std::size_t{};
        while (offset < input.size()) {
            const auto count = ::write(descriptor_, input.data() + offset, input.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return io_error("cannot write swtpm command");
            }
            offset += static_cast<std::size_t>(count);
        }
        return {};
    }

    auto read_all(std::span<std::byte> output) -> liquid::result<void> {
        auto offset = std::size_t{};
        while (offset < output.size()) {
            const auto count = ::read(descriptor_, output.data() + offset, output.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return io_error("cannot read swtpm response");
            }
            offset += static_cast<std::size_t>(count);
        }
        return {};
    }

    auto io_error(std::string message) -> liquid::result<void> {
        const auto detail = std::string{std::strerror(errno)};
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        return std::unexpected(
            liquid::error{liquid::errc::io, std::move(message), path_ + ": " + detail});
    }

    std::string path_;
    std::mutex mutex_;
    int descriptor_{-1};
};

auto extend_pcr(const std::shared_ptr<swtpm_transport>& transport, std::uint32_t pcr)
    -> liquid::result<void> {
    auto body = std::vector<std::byte>{};
    put_u32(body, pcr);
    put_u32(body, 9);
    put_u32(body, 0x40000009);
    put_u16(body, 0);
    body.push_back(std::byte{0});
    put_u16(body, 0);
    put_u32(body, 1);
    put_u16(body, 0x000b);
    body.insert(body.end(), 32, std::byte{0xa5});
    auto request = std::vector<std::byte>{std::byte{0x80}, std::byte{0x02}};
    put_u32(request, static_cast<std::uint32_t>(10 + body.size()));
    put_u32(request, 0x00000182);
    request.insert(request.end(), body.begin(), body.end());
    auto response = transport->submit(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->size() < 10 || get_u32(*response, 6) != 0) {
        return std::unexpected(liquid::error{
            liquid::errc::unauthorized, "swtpm PCR extend failed", "TPM2_PCR_Extend"});
    }
    return {};
}
#endif

auto set_environment(std::string_view name, const std::filesystem::path& value) -> void {
#if defined(_WIN32)
    EXPECT_EQ(_putenv_s(std::string{name}.c_str(), value.string().c_str()), 0);
#else
    EXPECT_EQ(setenv(std::string{name}.c_str(), value.c_str(), 1), 0);
#endif
}

auto identity_home(std::string_view name) -> std::filesystem::path {
    const auto path = liquid::test::fixture_path("liquid-hardware-" + std::string{name});
    std::error_code code;
    std::filesystem::remove_all(path, code);
    std::filesystem::create_directories(path, code);
    EXPECT_FALSE(code);
    set_environment("LIQUID_HOME", path);
    return path;
}

auto select_identity_home(std::string_view name) -> void {
    const auto path = liquid::test::fixture_path("liquid-hardware-" + std::string{name});
    set_environment("LIQUID_HOME", path);
}

auto open_identity(std::string_view name) -> liquid::signer {
    identity_home(name);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    return std::move(*signer);
}

// A verifier binds a signer's name to its key only through a chain it holds (liquid#383), so the
// current home trusts each signer's (single, initial) identity document.
auto trust_identities(std::initializer_list<const liquid::signer*> signers) -> void {
    for (const auto* value : signers) {
        const auto document = value->document();
        ASSERT_TRUE(document) << document.error().message;
        const auto trusted = liquid::trust_identity_chain(std::array{*document});
        ASSERT_TRUE(trusted) << trusted.error().message;
    }
}

auto test_signatures(std::string_view name, std::string_view destination) -> void {
    auto signer = open_identity(name);
    const auto original = signer.identity();
    auto rotated = liquid::runtime::block_on(signer.rotate_to(std::string{destination}));
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated->subject.taap_id, original.taap_id);
    EXPECT_NE(rotated->subject.key_fingerprint, original.key_fingerprint);
    for (const auto purpose : {std::string{"drip"}, std::string{"change"}}) {
        auto body = bytes(purpose + " body");
        auto signature = liquid::runtime::block_on(signer.sign(purpose, body));
        ASSERT_TRUE(signature) << signature.error().message;
        auto verified = liquid::verify(signer.identity(), purpose, body, *signature);
        ASSERT_TRUE(verified) << verified.error().message;
        EXPECT_TRUE(*verified);
    }
    auto reopened = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->identity().key_fingerprint, signer.identity().key_fingerprint);
}

auto digest_with(std::byte value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.fill(value);
    return output;
}

auto read_identity_bytes(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
    auto stream = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

auto expect_private_key_absent(std::string_view output, std::span<const std::byte> secret) -> void {
    ASSERT_GE(secret.size(), 32U);
    const auto raw = std::string_view{reinterpret_cast<const char*>(secret.data()), secret.size()};
    EXPECT_EQ(output.find(raw), std::string::npos);
    auto encoded = liquid::identity_detail::base64(secret);
    EXPECT_EQ(output.find(encoded), std::string::npos);
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    EXPECT_EQ(output.find(encoded), std::string::npos);
    auto hex = std::string{};
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    for (const auto byte : secret) {
        const auto value = std::to_integer<unsigned char>(byte);
        hex += digits[value >> 4];
        hex += digits[value & 15];
    }
    EXPECT_EQ(output.find(hex), std::string::npos);
}

auto expect_public_document(liquid::signer& signer, std::span<const std::byte> secret) -> void {
    auto show = std::string{"show"};
    auto document_flag = std::string{"--document"};
    auto arguments = std::array{show.data(), document_flag.data()};
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const auto status = liquid_cli::identity(static_cast<int>(arguments.size()), arguments.data());
    const auto output = testing::internal::GetCapturedStdout();
    const auto errors = testing::internal::GetCapturedStderr();
    ASSERT_EQ(status, 0) << errors;
    EXPECT_TRUE(errors.empty());
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(output.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"),
              std::string::npos);
    expect_private_key_absent(output, secret);

    auto padded = output;
    std::replace(padded.begin(), padded.end(), '-', '+');
    std::replace(padded.begin(), padded.end(), '_', '/');
    const auto padding = (4 - padded.size() % 4) % 4;
    padded.append(padding, '=');
    auto decoded = std::vector<std::byte>(padded.size() / 4 * 3);
    const auto count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                       reinterpret_cast<const unsigned char*>(padded.data()),
                                       static_cast<int>(padded.size()));
    ASSERT_GE(count, static_cast<int>(padding));
    decoded.resize(static_cast<std::size_t>(count) - padding);
    const auto raw =
        std::string_view{reinterpret_cast<const char*>(decoded.data()), decoded.size()};
    expect_private_key_absent(raw, secret);
    // Ed25519's 64-byte descriptor contains a 32-byte seed followed by the public key.
    if (secret.size() == 64) {
        expect_private_key_absent(output, secret.first(32));
        expect_private_key_absent(raw, secret.first(32));
    }
    const auto document = liquid::decode_identity_document(decoded);
    ASSERT_TRUE(document) << document.error().message;
    const auto verified = liquid::verify_document(*document);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(*verified);
    const auto held = signer.document();
    ASSERT_TRUE(held) << held.error().message;
    const auto canonical = liquid::encode_identity_document(*held);
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(decoded, *canonical);
    EXPECT_EQ(document->subject.taap_id, signer.identity().taap_id);
    EXPECT_EQ(document->subject.public_key, signer.identity().public_key);
    EXPECT_EQ(document->subject.custody, signer.identity().custody);
}

TEST(IdentityDocument, SoftwareOutputExcludesPrivateKey) {
    auto signer = open_identity("document-software");
    const auto home = liquid::test::fixture_path("liquid-hardware-document-software");
    const auto root = home / "identity";
    const auto wrapped = read_identity_bytes(root / (signer.identity().fingerprint + ".key"));
    const auto wrapping_key = read_identity_bytes(root / ".wrapping.key");
    ASSERT_EQ(wrapped.size(), 104U);
    ASSERT_EQ(wrapping_key.size(), 32U);
    auto secret = std::array<std::uint8_t, 64>{};
    ASSERT_EQ(crypto_aead_unlock(secret.data(),
                                 wrapped.data() + 24,
                                 wrapping_key.data(),
                                 wrapped.data(),
                                 nullptr,
                                 0,
                                 wrapped.data() + 40,
                                 secret.size()),
              0);
    expect_public_document(signer, std::as_bytes(std::span{secret}));
    const auto rotated = liquid::runtime::block_on(signer.rotate());
    ASSERT_TRUE(rotated) << rotated.error().message;
    const auto document = signer.document();
    ASSERT_TRUE(document) << document.error().message;
    EXPECT_EQ(document->subject.key_fingerprint, rotated->subject.key_fingerprint);
    expect_public_document(signer, std::as_bytes(std::span{secret}));
    crypto_wipe(secret.data(), secret.size());
}

TEST(IdentityDocument, Fido2OutputExcludesPrivateKey) {
    auto transport = std::make_shared<fido_double>();
    liquid::identity_detail::install_fido2_transport(transport);
    auto signer = open_identity("document-fido2");
    const auto rotated = liquid::runtime::block_on(signer.rotate_to("fido2"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    expect_public_document(signer, transport->private_key());
    liquid::identity_detail::install_fido2_transport({});
}

TEST(IdentityDocument, Fido2Es256OutputExcludesPrivateKey) {
    auto transport = std::make_shared<fido_es256_double>();
    liquid::identity_detail::install_fido2_transport(transport);
    auto signer = open_identity("document-fido2-es256");
    const auto rotated = liquid::runtime::block_on(signer.rotate_to("fido2"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    expect_public_document(signer, transport->private_key());
    liquid::identity_detail::install_fido2_transport({});
}

TEST(IdentityDocument, PivOutputExcludesPrivateKey) {
    auto transport = std::make_shared<piv_double>();
    liquid::identity_detail::install_piv_transport(transport);
    auto signer = open_identity("document-piv");
    const auto rotated = liquid::runtime::block_on(signer.rotate_to("piv:9c"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    expect_public_document(signer, transport->private_key());
    liquid::identity_detail::install_piv_transport({});
}

TEST(IdentityDocument, EmptySignerReturnsUsageError) {
    const auto signer = liquid::signer{};
    const auto document = signer.document();
    ASSERT_FALSE(document);
    EXPECT_EQ(document.error().code, liquid::errc::invalid_argument);
}

TEST(HardwareIdentity, fido2_double_signs_drips_and_changes) {
    liquid::identity_detail::install_fido2_transport(std::make_shared<fido_double>());
    test_signatures("fido", "fido2");
}

TEST(HardwareIdentity, fido2_es256_fallback_signs_drips_and_changes) {
    liquid::identity_detail::install_fido2_transport(std::make_shared<fido_es256_double>());
    test_signatures("fido-es256", "fido2");
}

// Independent vectors: what a real CTAP2 authenticator returns for liquid's getAssertion, built only
// from the specifications with OpenSSL. TAAP/1 signed bytes are "TAAP/1 " || purpose || 0x00 ||
// body; liquid sends clientDataHash = SHA-256(signed bytes); the authenticator signs
// authenticatorData || clientDataHash (WebAuthn Level 2 §6.3.3 step 11, CTAP 2.2 §6.2). EdDSA signs
// that message directly (RFC 8032); ES256 is ECDSA P-256 with SHA-256 over it (RFC 8152 §8.1).
class openssl_ed25519_key {
public:
    openssl_ed25519_key() {
        auto seed = std::array<unsigned char, 32>{};
        for (std::size_t index = 0; index < seed.size(); ++index) {
            seed[index] = static_cast<unsigned char>(0xa0 + index);
        }
        key_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
        EXPECT_NE(key_, nullptr);
    }

    ~openssl_ed25519_key() {
        EVP_PKEY_free(key_);
    }

    auto public_key() const -> std::vector<std::byte> {
        auto output = std::vector<std::byte>(32);
        auto size = output.size();
        EXPECT_GT(EVP_PKEY_get_raw_public_key(
                      key_, reinterpret_cast<unsigned char*>(output.data()), &size),
                  0);
        EXPECT_EQ(size, 32U);
        return output;
    }

    auto sign_message(std::span<const std::byte> message) const -> std::vector<std::byte> {
        auto context = EVP_MD_CTX_new();
        EXPECT_NE(context, nullptr);
        EXPECT_GT(EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key_), 0);
        auto output = std::vector<std::byte>(64);
        auto size = output.size();
        EXPECT_GT(EVP_DigestSign(context,
                                 reinterpret_cast<unsigned char*>(output.data()),
                                 &size,
                                 reinterpret_cast<const unsigned char*>(message.data()),
                                 message.size()),
                  0);
        EVP_MD_CTX_free(context);
        EXPECT_EQ(size, 64U);
        return output;
    }

private:
    EVP_PKEY* key_{};
};

auto taap_signed_bytes(std::string_view purpose, std::span<const std::byte> body)
    -> std::vector<std::byte> {
    auto output = bytes("TAAP/1 " + std::string{purpose});
    output.push_back(std::byte{0});
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

struct fido2_vector {
    liquid::actor by;
    liquid::signature correct;
    liquid::signature double_hashed;
    liquid::signature other_relying_party;
    liquid::signature user_absent;
};

template <typename Sign>
auto make_fido2_vector(std::string algorithm,
                       std::vector<std::byte> public_key,
                       std::string_view purpose,
                       std::span<const std::byte> body,
                       Sign sign) -> fido2_vector {
    auto by = liquid::actor{};
    by.algorithm = std::move(algorithm);
    by.fingerprint = std::string(52, 'V');
    by.key_fingerprint = std::string(52, 'W');
    by.taap_id = "taap:1:" + by.fingerprint;
    by.public_key = std::move(public_key);
    by.custody = "fido2";
    const auto client_data_hash = openssl_sha256(taap_signed_bytes(purpose, body));
    const auto authenticator_data = ctap2_assertion_authenticator_data("liquid", 7);

    auto correct_input = authenticator_data;
    correct_input.insert(correct_input.end(), client_data_hash.begin(), client_data_hash.end());
    // The form liquid verified before liquid#81 was fixed: the client hash hashed a second time.
    const auto rehashed = openssl_sha256(client_data_hash);
    auto double_hashed_input = authenticator_data;
    double_hashed_input.insert(double_hashed_input.end(), rehashed.begin(), rehashed.end());

    auto make = [&](std::vector<std::byte> signature_bytes, std::vector<std::byte> data) {
        return liquid::signature{by.algorithm,
                                 "taap:1:" + by.algorithm + ":" + by.key_fingerprint,
                                 "fido2",
                                 std::string{purpose},
                                 "2026-10-20T00:00:00Z",
                                 std::move(signature_bytes),
                                 std::move(data),
                                 client_data_hash};
    };
    // Correctly signed assertions whose authenticator data fails WebAuthn §7.2 steps 15-16.
    auto signed_with = [&](std::vector<std::byte> data) {
        auto input = data;
        input.insert(input.end(), client_data_hash.begin(), client_data_hash.end());
        return make(sign(input), std::move(data));
    };
    return {by,
            make(sign(correct_input), authenticator_data),
            make(sign(double_hashed_input), authenticator_data),
            signed_with(ctap2_assertion_authenticator_data("example.com", 7)),
            signed_with(ctap2_assertion_authenticator_data("liquid", 7, std::byte{0x00}))};
}

auto expect_fido2_vector(const fido2_vector& vector,
                         std::string_view purpose,
                         std::span<const std::byte> body) -> void {
    auto verified = liquid::verify(vector.by, purpose, body, vector.correct);
    ASSERT_TRUE(verified) << vector.by.algorithm << ": " << verified.error().message;
    EXPECT_TRUE(*verified);

    auto rehashed = liquid::verify(vector.by, purpose, body, vector.double_hashed);
    ASSERT_FALSE(rehashed) << vector.by.algorithm << " accepted a double-hashed client hash";
    EXPECT_EQ(rehashed.error().code, liquid::errc::unauthorized);

    for (const auto* refused : {&vector.other_relying_party, &vector.user_absent}) {
        auto result = liquid::verify(vector.by, purpose, body, *refused);
        ASSERT_FALSE(result) << vector.by.algorithm << " accepted invalid authenticator data";
        EXPECT_EQ(result.error().code, liquid::errc::unauthorized);
    }

    auto tampered = vector.correct;
    tampered.authenticator_data[32] = std::byte{0x05};
    auto tampered_result = liquid::verify(vector.by, purpose, body, tampered);
    ASSERT_FALSE(tampered_result);
    EXPECT_EQ(tampered_result.error().code, liquid::errc::unauthorized);

    auto other_body = bytes("another body");
    auto wrong_body = liquid::verify(vector.by, purpose, other_body, vector.correct);
    ASSERT_FALSE(wrong_body);
    EXPECT_EQ(wrong_body.error().code, liquid::errc::unauthorized);
}

TEST(HardwareIdentity, fido2_assertion_vectors_follow_ctap2_signed_bytes) {
    identity_home("fido2-vectors");
    const auto ed25519 = openssl_ed25519_key{};
    const auto p256 = p256_key{};
    for (const auto purpose : {std::string{"drip"},
                               std::string{"change"},
                               std::string{"identity_document"},
                               std::string{"attestation"}}) {
        const auto body = bytes(purpose + " canonical body");
        expect_fido2_vector(
            make_fido2_vector("fido2-eddsa",
                              ed25519.public_key(),
                              purpose,
                              body,
                              [&](std::span<const std::byte> message) {
                                  return ed25519.sign_message(message);
                              }),
            purpose,
            body);
        expect_fido2_vector(
            make_fido2_vector("fido2-es256",
                              p256.public_key(),
                              purpose,
                              body,
                              [&](std::span<const std::byte> message) {
                                  return p256.sign_message(message);
                              }),
            purpose,
            body);
    }
}

TEST(HardwareIdentity, piv_double_signs_drips_and_changes) {
    liquid::identity_detail::install_piv_transport(std::make_shared<piv_double>());
    test_signatures("piv", "piv:9c");
}

TEST(HardwareIdentity, wrong_algorithm_and_malformed_metadata_are_unauthorized) {
    auto signer = open_identity("wrong-algorithm");
    auto body = bytes("body");
    auto signed_value = liquid::runtime::block_on(signer.sign("drip", body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    signed_value->algorithm = "ecdsa-p256";
    auto wrong = liquid::verify(signer.identity(), "drip", body, *signed_value);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, liquid::errc::unauthorized);
    signed_value->algorithm = "ed25519";
    signed_value->bytes.resize(12);
    auto malformed = liquid::verify(signer.identity(), "drip", body, *signed_value);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, liquid::errc::unauthorized);
}

TEST(ReleaseApproval, threshold_counts_distinct_valid_identities) {
    auto policy_signer = open_identity("release-policy");
    auto first = open_identity("release-first");
    auto second = open_identity("release-second");
    auto third = open_identity("release-third");
    const auto repository = digest_with(std::byte{0x11});
    const auto artifact = digest_with(std::byte{0x22});
    auto policy = liquid::runtime::block_on(liquid::make_release_policy(
        repository,
        "targets",
        2,
        {first.identity().taap_id, second.identity().taap_id, third.identity().taap_id},
        "2099-01-01T00:00:00Z",
        policy_signer));
    ASSERT_TRUE(policy) << policy.error().message;
    auto first_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v1", first));
    auto second_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v1", second));
    ASSERT_TRUE(first_approval) << first_approval.error().message;
    ASSERT_TRUE(second_approval) << second_approval.error().message;

    // The verifier runs in `third`'s home. Until it trusts the other identities, the policy's
    // issuer is unknown and the policy is refused.
    const auto both = std::array{*first_approval, *second_approval};
    auto unknown_policy = liquid::verify_release(artifact, *policy, both);
    ASSERT_FALSE(unknown_policy);
    EXPECT_EQ(unknown_policy.error().code, liquid::errc::unauthorized);
    trust_identities({&policy_signer});
    auto unknown_approvers = liquid::verify_release(artifact, *policy, both);
    ASSERT_TRUE(unknown_approvers) << unknown_approvers.error().message;
    EXPECT_FALSE(unknown_approvers->satisfied);
    EXPECT_TRUE(unknown_approvers->distinct_valid_signers.empty());
    auto expected_unknown =
        std::vector<std::string>{first.identity().taap_id, second.identity().taap_id};
    std::ranges::sort(expected_unknown);
    EXPECT_EQ(unknown_approvers->unknown_signers, expected_unknown);
    trust_identities({&first, &second});

    const auto one = std::array{*first_approval};
    auto one_result = liquid::verify_release(artifact, *policy, one);
    ASSERT_TRUE(one_result) << one_result.error().message;
    EXPECT_FALSE(one_result->satisfied);

    const auto duplicate = std::array{*first_approval, *first_approval};
    auto duplicate_result = liquid::verify_release(artifact, *policy, duplicate);
    ASSERT_TRUE(duplicate_result) << duplicate_result.error().message;
    EXPECT_FALSE(duplicate_result->satisfied);
    EXPECT_EQ(duplicate_result->distinct_valid_signers.size(), 1U);

    const auto two = std::array{*first_approval, *second_approval};
    auto two_result = liquid::verify_release(artifact, *policy, two);
    ASSERT_TRUE(two_result) << two_result.error().message;
    EXPECT_TRUE(two_result->satisfied);
    EXPECT_EQ(two_result->distinct_valid_signers.size(), 2U);
}

// liquid#429, second exploit: one person forges release_approval/1 statements that name two
// listed signers, each self-signed with a throwaway key. They never count toward the threshold:
// not while the verifier lacks the named signers' chains, not once it holds them, and not when it
// also trusts the throwaway identities.
TEST(ReleaseApproval, self_signed_approvals_naming_listed_signers_do_not_count) {
    auto policy_signer = open_identity("forged-policy");
    auto first = open_identity("forged-first");
    auto second = open_identity("forged-second");
    auto third = open_identity("forged-third");
    auto throwaway_one = open_identity("forged-throwaway-one");
    auto throwaway_two = open_identity("forged-throwaway-two");
    const auto repository = digest_with(std::byte{0x55});
    const auto artifact = digest_with(std::byte{0x66});
    auto policy = liquid::runtime::block_on(liquid::make_release_policy(
        repository,
        "targets",
        2,
        {first.identity().taap_id, second.identity().taap_id, third.identity().taap_id},
        "2099-01-01T00:00:00Z",
        policy_signer));
    ASSERT_TRUE(policy) << policy.error().message;

    // The issuer names a listed signer; the key, and the signature, are the throwaway's.
    const auto forge =
        [&](liquid::signer& throwaway,
            const liquid::actor& claimed) -> liquid::result<liquid::release_approval> {
        auto approval =
            liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v1", throwaway));
        if (!approval) {
            return std::unexpected(approval.error());
        }
        auto statement = approval->statement;
        statement.issuer.taap_id = claimed.taap_id;
        statement.issuer.fingerprint = claimed.fingerprint;
        auto body = liquid::objects::encode_attestation_body_unchecked(statement);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto signed_value = liquid::runtime::block_on(throwaway.sign("attestation", *body));
        if (!signed_value) {
            return std::unexpected(signed_value.error());
        }
        auto encoded = liquid::encode_attestation_signature(*signed_value);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        statement.signature = std::move(*encoded);
        return liquid::release_approval{std::move(statement)};
    };
    auto forged_first = forge(throwaway_one, first.identity());
    auto forged_second = forge(throwaway_two, second.identity());
    ASSERT_TRUE(forged_first) << forged_first.error().message;
    ASSERT_TRUE(forged_second) << forged_second.error().message;
    const auto forged = std::array{*forged_first, *forged_second};

    select_identity_home("forged-third");
    trust_identities({&policy_signer});
    auto unknown = liquid::verify_release(artifact, *policy, forged);
    ASSERT_TRUE(unknown) << unknown.error().message;
    EXPECT_FALSE(unknown->satisfied);
    EXPECT_TRUE(unknown->distinct_valid_signers.empty());

    trust_identities({&first, &second, &throwaway_one, &throwaway_two});
    auto known = liquid::verify_release(artifact, *policy, forged);
    ASSERT_TRUE(known) << known.error().message;
    EXPECT_FALSE(known->satisfied) << "self-signed approvals met a 2-of-3 threshold";
    EXPECT_TRUE(known->distinct_valid_signers.empty());
    EXPECT_TRUE(known->unknown_signers.empty());

    // A real approval counts once; the forged one beside it still adds nothing.
    auto real_first =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v1", first));
    ASSERT_TRUE(real_first) << real_first.error().message;
    const auto mixed = std::array{*real_first, *forged_second};
    auto one = liquid::verify_release(artifact, *policy, mixed);
    ASSERT_TRUE(one) << one.error().message;
    EXPECT_FALSE(one->satisfied);
    EXPECT_EQ(one->distinct_valid_signers, std::vector<std::string>{first.identity().taap_id});
}

TEST(ReleaseApproval, fido2_eddsa_and_es256_approvals_meet_the_threshold) {
    auto policy_signer = open_identity("fido2-release-policy");
    liquid::identity_detail::install_fido2_transport(std::make_shared<fido_double>());
    auto first = open_identity("fido2-release-eddsa");
    ASSERT_TRUE(liquid::runtime::block_on(first.rotate_to("fido2")));
    ASSERT_EQ(first.identity().algorithm, "fido2-eddsa");
    liquid::identity_detail::install_fido2_transport(std::make_shared<fido_es256_double>());
    auto second = open_identity("fido2-release-es256");
    ASSERT_TRUE(liquid::runtime::block_on(second.rotate_to("fido2")));
    ASSERT_EQ(second.identity().algorithm, "fido2-es256");
    const auto repository = digest_with(std::byte{0x55});
    const auto artifact = digest_with(std::byte{0x66});
    auto policy = liquid::runtime::block_on(
        liquid::make_release_policy(repository,
                                    "targets",
                                    2,
                                    {first.identity().taap_id, second.identity().taap_id},
                                    "2099-01-01T00:00:00Z",
                                    policy_signer));
    ASSERT_TRUE(policy) << policy.error().message;
    auto first_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v3", first));
    auto second_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v3", second));
    ASSERT_TRUE(first_approval) << first_approval.error().message;
    ASSERT_TRUE(second_approval) << second_approval.error().message;
    const auto approvals = std::array{*first_approval, *second_approval};
    auto result = liquid::verify_release(artifact, *policy, approvals);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->satisfied);
    EXPECT_EQ(result->distinct_valid_signers.size(), 2U);
    liquid::identity_detail::install_fido2_transport({});
}

TEST(ReleaseApproval, approval_after_declared_compromise_is_rejected) {
    auto policy_signer = open_identity("compromise-policy");
    auto first = open_identity("compromise-first");
    auto second = open_identity("compromise-second");
    const auto repository = digest_with(std::byte{0x33});
    const auto artifact = digest_with(std::byte{0x44});
    auto policy = liquid::runtime::block_on(
        liquid::make_release_policy(repository,
                                    "targets",
                                    2,
                                    {first.identity().taap_id, second.identity().taap_id},
                                    "2099-01-01T00:00:00Z",
                                    policy_signer));
    ASSERT_TRUE(policy) << policy.error().message;
    auto first_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v2", first));
    auto second_approval =
        liquid::runtime::block_on(liquid::approve_release(artifact, *policy, "v2", second));
    ASSERT_TRUE(first_approval) << first_approval.error().message;
    ASSERT_TRUE(second_approval) << second_approval.error().message;
    auto signature = liquid::decode_signature(second_approval->statement.signature);
    ASSERT_TRUE(signature) << signature.error().message;
    select_identity_home("compromise-second");
    auto rotated = liquid::runtime::block_on(second.rotate(liquid::compromise_declaration{
        second.identity().key_fingerprint, signature->signed_at, "release key compromised"}));
    ASSERT_TRUE(rotated) << rotated.error().message;
    trust_identities({&policy_signer, &first});
    const auto approvals = std::array{*first_approval, *second_approval};
    auto result = liquid::verify_release(artifact, *policy, approvals);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(result->satisfied);
    EXPECT_EQ(result->distinct_valid_signers.size(), 1U);
}

TEST(TpmCustody, seal_unseal_and_changed_pcr_refusal) {
    auto transport = std::make_shared<tpm_double>();
    liquid::custody_detail::install_tpm_transport(transport);
    auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri("tpm:pcrs=0,7"));
    ASSERT_TRUE(provider) << provider.error().message;
    auto source = liquid::key{};
    source.fill(std::byte{0x5a});
    auto wrapped = liquid::runtime::block_on((*provider)->wrap(source, {"build", 1}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    auto opened = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"build", 1}));
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, source);
    transport->change_pcr();
    auto refused = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"build", 1}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}

#if defined(__linux__)
TEST(TpmCustody, swtpm_seals_unseals_and_refuses_a_changed_pcr) {
    const auto* socket = std::getenv("LIQUID_SWTPM_SOCKET");
    if (!socket || socket[0] == '\0') {
        GTEST_SKIP() << "LIQUID_SWTPM_SOCKET is not configured";
    }
    auto transport = std::make_shared<swtpm_transport>(socket);
    liquid::custody_detail::install_tpm_transport(transport);
    auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri("tpm:pcrs=0,7"));
    ASSERT_TRUE(provider) << provider.error().message;
    auto source = liquid::key{};
    source.fill(std::byte{0x6b});
    auto wrapped = liquid::runtime::block_on((*provider)->wrap(source, {"swtpm", 1}));
    ASSERT_TRUE(wrapped) << wrapped.error().message << " (" << wrapped.error().context << ")";
    auto opened = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"swtpm", 1}));
    ASSERT_TRUE(opened) << opened.error().message << " (" << opened.error().context << ")";
    EXPECT_EQ(*opened, source);
    auto extended = extend_pcr(transport, 7);
    ASSERT_TRUE(extended) << extended.error().message;
    auto refused = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"swtpm", 1}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}
#endif

#if defined(__APPLE__)
TEST(TpmCustody, macos_reports_the_platform_reason) {
    liquid::custody_detail::install_tpm_transport({});
    auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri("tpm:pcrs=0,7"));
    ASSERT_TRUE(provider) << provider.error().message;
    auto source = liquid::key{};
    auto wrapped = liquid::runtime::block_on((*provider)->wrap(source, {"build", 1}));
    ASSERT_FALSE(wrapped);
    EXPECT_EQ(wrapped.error().code, liquid::errc::unsupported);
    EXPECT_NE(wrapped.error().message.find("Secure Enclave"), std::string::npos);
}
#endif

} // namespace
