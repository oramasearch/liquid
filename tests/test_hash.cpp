// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <span>
#include <string_view>
#include <type_traits>

#include "liquid/hash.hpp"

namespace {

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

void expect_hash(liquid::algorithm algorithm, std::string_view input, std::string_view expected) {
    const auto value = liquid::hash(algorithm, bytes(input));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(liquid::to_string(*value), expected);
}

} // namespace

TEST(Hash, Blake3KnownVectors) {
    static_assert(std::is_aggregate_v<liquid::digest>);
    expect_hash(liquid::algorithm::blake3,
                "",
                "blake3:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    expect_hash(liquid::algorithm::blake3,
                "abc",
                "blake3:6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

TEST(Hash, Sha256KnownVectors) {
    expect_hash(liquid::algorithm::sha256,
                "",
                "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_hash(liquid::algorithm::sha256,
                "abc",
                "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Hash, DigestTextRoundTrips) {
    for (const auto algorithm : {liquid::algorithm::blake3, liquid::algorithm::sha256}) {
        const auto hashed = liquid::hash(algorithm, bytes("round trip"));
        ASSERT_TRUE(hashed.has_value());
        const auto parsed = liquid::parse_digest(liquid::to_string(*hashed));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->alg, hashed->alg);
        EXPECT_EQ(parsed->bytes, hashed->bytes);
    }
    liquid::digest keyed{liquid::algorithm::blake3_keyed, {}};
    keyed.bytes[0] = std::byte{0xab};
    const auto parsed = liquid::parse_digest(liquid::to_string(keyed));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->alg, liquid::algorithm::blake3_keyed);
    EXPECT_EQ(parsed->bytes, keyed.bytes);
}

TEST(Hash, RejectsInvalidTextAndAlgorithm) {
    const auto bad_prefix = liquid::parse_digest("md5:00000000000000000000000000000000");
    ASSERT_FALSE(bad_prefix.has_value());
    EXPECT_EQ(bad_prefix.error().code, liquid::errc::unsupported);
    EXPECT_EQ(bad_prefix.error().context, "md5");

    const auto bad_length = liquid::parse_digest("blake3:00");
    ASSERT_FALSE(bad_length.has_value());
    EXPECT_EQ(bad_length.error().code, liquid::errc::invalid_argument);

    const auto bad_hex = liquid::parse_digest(
        "sha256:gg00000000000000000000000000000000000000000000000000000000000000");
    ASSERT_FALSE(bad_hex.has_value());
    EXPECT_EQ(bad_hex.error().code, liquid::errc::invalid_argument);

    const auto bad_algorithm = liquid::hash(static_cast<liquid::algorithm>(0), {});
    ASSERT_FALSE(bad_algorithm.has_value());
    EXPECT_EQ(bad_algorithm.error().code, liquid::errc::invalid_argument);
}
