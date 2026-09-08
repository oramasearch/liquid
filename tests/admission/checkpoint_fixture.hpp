#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include "liquid/hash.hpp"
#include "liquid/transparency.hpp"

namespace liquid::test::admission {

inline auto bytes(std::string_view text) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

inline auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto byte : input) {
        const auto value = std::to_integer<unsigned char>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 15U]);
    }
    return output;
}

inline auto base64url(std::span<const std::byte> input) -> std::string {
    constexpr auto digits =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
    auto output = std::string{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto byte : input) {
        accumulator = (accumulator << 8U) | std::to_integer<unsigned char>(byte);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(digits[(accumulator >> bits) & 63U]);
        }
    }
    if (bits != 0) {
        output.push_back(digits[(accumulator << (6U - bits)) & 63U]);
    }
    return output;
}

inline auto sha256(std::span<const std::byte> input) -> transparency::hash {
    const auto digest = liquid::hash(algorithm::sha256, input);
    EXPECT_TRUE(digest.has_value());
    return digest ? digest->bytes : transparency::hash{};
}

struct signed_checkpoint {
    transparency::checkpoint checkpoint;
    transparency::log_trust trust;
    nlohmann::json record;
};

inline auto sign_checkpoint(const nlohmann::json& body) -> signed_checkpoint {
    auto seed = std::array<std::uint8_t, 32>{42};
    auto secret = std::array<std::uint8_t, 64>{};
    auto public_key = std::array<std::byte, 32>{};
    crypto_ed25519_key_pair(
        secret.data(), reinterpret_cast<std::uint8_t*>(public_key.data()), seed.data());
    auto input = bytes("TAAP/1 lake_checkpoint/1");
    input.push_back(std::byte{});
    const auto cbor = nlohmann::json::to_cbor(body);
    for (const auto byte : cbor) {
        input.push_back(std::byte{byte});
    }
    auto signature = std::vector<std::byte>(64);
    crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()),
                        secret.data(),
                        reinterpret_cast<const std::uint8_t*>(input.data()),
                        input.size());
    const auto canonical = bytes(body.dump());
    const auto digest = sha256(canonical);
    const auto root = liquid::parse_digest("sha256:" + body.at("root_hash").get<std::string>());
    EXPECT_TRUE(root.has_value());
    const auto signer = std::string{"taap:1:ed25519:admission-clock-fixture"};
    return {
        .checkpoint = {.sequence = 1,
                       .tree_size = body.at("tree_size").get<std::uint64_t>(),
                       .entry_tile_width = 1024,
                       .hash_tile_width = 1024,
                       .root_hash = root ? root->bytes : transparency::hash{},
                       .checkpoint_hash = digest,
                       .signature = {signer, signature},
                       .canonical_body = canonical},
        .trust = {.identity = {.taap_id = signer,
                               .algorithm = "ed25519",
                               .public_key = {public_key.begin(), public_key.end()}},
                  .configuration = {.witness_threshold = 0}},
        .record = {
            {"checkpoint", body},
            {"sequence", 1},
            {"checkpoint_hash_sha256", hex(digest)},
            {"signature",
             {{"algorithm", "ed25519"}, {"signer", signer}, {"signature", base64url(signature)}}},
            {"witness_signatures", nlohmann::json::array()}}};
}

} // namespace liquid::test::admission
