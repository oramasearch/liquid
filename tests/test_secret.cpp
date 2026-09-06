#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crypto/crypto.hpp"
#include "liquid/runtime.hpp"
#include "liquid/secret.hpp"
#include "net/http_client.hpp"
#include "secret/secret.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto json_body(std::string_view value) -> std::vector<std::byte> {
    return bytes(value);
}

auto hex_value(char character) -> int {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

auto decode_hex(std::string text) -> std::vector<std::byte> {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    EXPECT_EQ(text.size() % 2, 0U);
    auto output = std::vector<std::byte>{};
    output.reserve(text.size() / 2);
    for (auto offset = std::size_t{}; offset + 1 < text.size(); offset += 2) {
        const auto high = hex_value(text[offset]);
        const auto low = hex_value(text[offset + 1]);
        EXPECT_GE(high, 0);
        EXPECT_GE(low, 0);
        output.push_back(std::byte{static_cast<unsigned char>((high << 4) | low)});
    }
    return output;
}

auto fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_SECRET_FIXTURES} / name;
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open()) << path;
    return decode_hex(std::string{std::istreambuf_iterator<char>{input}, {}});
}

template <std::size_t Size>
auto fixed(std::span<const std::byte> input) -> std::array<std::byte, Size> {
    EXPECT_EQ(input.size(), Size);
    auto output = std::array<std::byte, Size>{};
    std::ranges::copy(input, output.begin());
    return output;
}

auto deterministic_random(std::vector<std::byte> input) -> liquid::secret_detail::random_source {
    auto source = std::make_shared<std::pair<std::vector<std::byte>, std::size_t>>(std::move(input),
                                                                                   std::size_t{});
    return [source](std::span<std::byte> output) -> liquid::result<void> {
        if (source->first.size() - source->second < output.size()) {
            return std::unexpected(liquid::error{
                liquid::errc::internal, "deterministic source exhausted", "sealed-secret fixture"});
        }
        std::ranges::copy(
            std::span<const std::byte>{source->first}.subspan(source->second, output.size()),
            output.begin());
        source->second += output.size();
        return {};
    };
}

auto fixed_keypair() -> liquid::sealing_keypair {
    const auto private_bytes = bytes("0123456789abcdef0123456789abcdef");
    auto private_key = fixed<32>(private_bytes);
    auto public_key = liquid::crypto_detail::x25519_public_key(private_key);
    EXPECT_TRUE(public_key);
    return liquid::sealing_keypair{
        .public_key = liquid::sealing_public_key{.kid = "lake-key-2026-09", .key = *public_key},
        .private_key = private_key,
    };
}

auto full_scope() -> liquid::seal_scope {
    return {.cluster = "lake-prod", .namespace_ = "payments", .name = "database"};
}

auto expect_unauthorized(const liquid::result<std::vector<std::byte>>& result,
                         std::string_view context) -> void {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(result.error().context, context);
}

TEST(SealedSecret, round_trips_and_generates_x25519_keypairs) {
    auto generated = liquid::generate_sealing_keypair();
    ASSERT_TRUE(generated);
    auto public_key = liquid::crypto_detail::x25519_public_key(generated->private_key);
    ASSERT_TRUE(public_key);
    EXPECT_EQ(generated->public_key.key, *public_key);
    EXPECT_TRUE(generated->public_key.kid.empty());

    generated->public_key.kid = "opaque-operator-kid";
    const auto scope = full_scope();
    const auto plaintext = bytes("not returned before authentication");
    auto sealed = liquid::seal_secret(generated->public_key, scope, plaintext);
    ASSERT_TRUE(sealed);
    auto encoded = liquid::encode_sealed_secret(*sealed);
    ASSERT_TRUE(encoded);
    auto decoded = liquid::decode_sealed_secret(*encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, *sealed);
    auto opened = liquid::unseal_secret(generated->private_key, *decoded, scope);
    ASSERT_TRUE(opened);
    EXPECT_EQ(*opened, plaintext);
}

TEST(SealedSecret, rejects_noncanonical_cbor) {
    auto pair = fixed_keypair();
    auto sealed = liquid::seal_secret(pair.public_key, full_scope(), bytes("value"));
    ASSERT_TRUE(sealed);
    auto encoded = liquid::encode_sealed_secret(*sealed);
    ASSERT_TRUE(encoded);
    ASSERT_EQ((*encoded)[0], std::byte{0xa6});
    (*encoded)[0] = std::byte{0xb8};
    encoded->insert(encoded->begin() + 1, std::byte{0x06});
    auto decoded = liquid::decode_sealed_secret(*encoded);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, liquid::errc::corrupt);
}

TEST(SealedSecret, authenticates_every_record_field) {
    auto pair = fixed_keypair();
    const auto scope = full_scope();
    auto sealed = liquid::seal_secret(pair.public_key, scope, bytes("value"));
    ASSERT_TRUE(sealed);

    auto tampered = *sealed;
    tampered.kid += "-other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, scope), "ciphertext");

    tampered = *sealed;
    tampered.epk[0] ^= std::byte{1};
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, scope), "ciphertext");

    tampered = *sealed;
    tampered.nonce[0] ^= std::byte{1};
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, scope), "ciphertext");

    tampered = *sealed;
    tampered.scope.cluster += "-other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, tampered.scope),
                        "ciphertext");

    tampered = *sealed;
    tampered.scope.namespace_ = "other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, tampered.scope),
                        "ciphertext");

    tampered = *sealed;
    tampered.scope.name = "other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, tampered.scope),
                        "ciphertext");

    tampered = *sealed;
    tampered.ciphertext[0] ^= std::byte{1};
    expect_unauthorized(liquid::unseal_secret(pair.private_key, tampered, scope), "ciphertext");
}

TEST(SealedSecret, rejects_exact_scope_mismatches) {
    auto pair = fixed_keypair();
    const auto scope = full_scope();
    auto sealed = liquid::seal_secret(pair.public_key, scope, bytes("value"));
    ASSERT_TRUE(sealed);

    auto expected = scope;
    expected.cluster = "other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *sealed, expected), "cluster");

    expected = scope;
    expected.namespace_ = "other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *sealed, expected), "namespace");

    expected = scope;
    expected.name = "other";
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *sealed, expected), "name");

    expected = scope;
    expected.namespace_.reset();
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *sealed, expected), "namespace");

    expected = scope;
    expected.name.reset();
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *sealed, expected), "name");

    auto absent_scope = liquid::seal_scope{.cluster = scope.cluster};
    auto absent = liquid::seal_secret(pair.public_key, absent_scope, bytes("value"));
    ASSERT_TRUE(absent);
    expect_unauthorized(liquid::unseal_secret(pair.private_key, *absent, scope), "namespace");
}

TEST(SealedSecret, rejects_wrong_private_key_and_all_zero_shared_secret) {
    auto pair = fixed_keypair();
    auto other = liquid::generate_sealing_keypair();
    ASSERT_TRUE(other);
    const auto scope = full_scope();
    auto sealed = liquid::seal_secret(pair.public_key, scope, bytes("value"));
    ASSERT_TRUE(sealed);
    expect_unauthorized(liquid::unseal_secret(other->private_key, *sealed, scope), "ciphertext");

    auto deterministic = deterministic_random(bytes("0123456789abcdef0123456789abcdef"
                                                    "nonce-for-liquid-fixture"));
    auto invalid = liquid::secret_detail::seal_secret_with_random(
        liquid::sealing_public_key{.kid = "zero", .key = {}}, scope, bytes("value"), deterministic);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(invalid.error().context, "public_key");
}

TEST(SealedSecret, fixture_decodes_unseals_and_reseals_byte_identically) {
    const auto private_key = fixed<32>(fixture("recipient-private.hex"));
    const auto public_key = fixed<32>(fixture("recipient-public.hex"));
    const auto ephemeral_private = fixture("ephemeral-private.hex");
    const auto nonce = fixture("nonce.hex");
    const auto plaintext = fixture("plaintext.hex");
    const auto record = fixture("record.cbor.hex");
    const auto scope =
        liquid::seal_scope{.cluster = "lake-prod", .namespace_ = "payments", .name = "database"};

    auto decoded = liquid::decode_sealed_secret(record);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->epk, fixed<32>(fixture("ephemeral-public.hex")));
    EXPECT_EQ(decoded->nonce, fixed<24>(nonce));
    auto opened = liquid::unseal_secret(private_key, *decoded, scope);
    ASSERT_TRUE(opened);
    EXPECT_EQ(*opened, plaintext);

    auto random_bytes = ephemeral_private;
    random_bytes.insert(random_bytes.end(), nonce.begin(), nonce.end());
    auto resealed = liquid::secret_detail::seal_secret_with_random(
        liquid::sealing_public_key{.kid = decoded->kid, .key = public_key},
        scope,
        plaintext,
        deterministic_random(std::move(random_bytes)));
    ASSERT_TRUE(resealed);
    auto encoded = liquid::encode_sealed_secret(*resealed);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, record);
}

TEST(SealedSecret, fetches_lake_public_key_through_the_private_http_client) {
    auto captured = std::make_shared<std::optional<liquid::net_detail::http_request>>();
    const auto response = liquid::net_detail::http_response{
        .status = 200,
        .body = json_body(
            R"({"ok":true,"value":{"kid":"operator-kid","public_key":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f","algorithm":"x25519"},"duration_ms":1})"),
    };
    auto requester = [captured, response](liquid::net_detail::http_request request)
        -> liquid::runtime::task<liquid::net_detail::http_response> {
        *captured = std::move(request);
        co_return response;
    };
    auto fetched = liquid::runtime::block_on(liquid::secret_detail::fetch_sealing_public_key_with(
        "https://lake.example/base/", "bearer-value", requester));
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->kid, "operator-kid");
    EXPECT_EQ(fetched->key[31], std::byte{0x1f});
    ASSERT_TRUE(captured->has_value());
    EXPECT_EQ((*captured)->method, liquid::net_detail::http_method::get);
    EXPECT_EQ((*captured)->url.host, "lake.example");
    EXPECT_EQ((*captured)->target, "/base/v1/sealing_public_key");
    EXPECT_EQ((*captured)->bearer, "bearer-value");
}

TEST(SealedSecret, maps_lake_error_envelopes) {
    struct example {
        unsigned status;
        std::string_view code;
        liquid::errc expected;
    };
    for (const auto& item : {
             example{400, "invalid_input", liquid::errc::invalid_argument},
             example{403, "unauthorized", liquid::errc::unauthorized},
             example{404, "not_found", liquid::errc::not_found},
             example{409, "conflict", liquid::errc::conflict},
             example{503, "store_unavailable", liquid::errc::unavailable},
         }) {
        auto body = std::string{R"({"ok":false,"error":{"code":")"} + std::string{item.code} +
                    R"(","message":"refused","path":"sealing_public_key"},"duration_ms":1})";
        auto requester = [status = item.status,
                          body = json_body(body)](liquid::net_detail::http_request)
            -> liquid::runtime::task<liquid::net_detail::http_response> {
            co_return liquid::net_detail::http_response{.status = status, .body = body};
        };
        auto fetched =
            liquid::runtime::block_on(liquid::secret_detail::fetch_sealing_public_key_with(
                "http://lake.example", {}, requester));
        ASSERT_FALSE(fetched);
        EXPECT_EQ(fetched.error().code, item.expected);
        EXPECT_EQ(fetched.error().context, "sealing_public_key");
    }
}

} // namespace
