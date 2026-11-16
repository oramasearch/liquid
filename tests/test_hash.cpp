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

// liquid#413: the project's own SHA-256 and SHA-1 agree with OpenSSL's on every length around each
// block and padding boundary, and on random input, so a boundary bug cannot corrupt identities
// silently.
#include <openssl/evp.h>

#include <random>
#include <vector>

#include "git/sha1.hpp"

TEST(Hash, Sha256AndSha1MatchOpenSslAcrossBlockBoundaries) {
    auto generator = std::mt19937_64{413};
    const auto openssl = [](const EVP_MD* digest, std::span<const std::byte> input) {
        auto output = std::vector<unsigned char>(EVP_MAX_MD_SIZE);
        auto size = 0U;
        EXPECT_EQ(EVP_Digest(input.data(), input.size(), output.data(), &size, digest, nullptr), 1);
        output.resize(size);
        return output;
    };
    auto lengths = std::vector<std::size_t>{};
    for (std::size_t length = 0; length <= 1100; ++length) {
        lengths.push_back(length);  // every length through 17 blocks: each 55/56/63/64 edge
    }
    for (int extra = 0; extra < 64; ++extra) {
        lengths.push_back(std::uniform_int_distribution<std::size_t>{1101, 1U << 20}(generator));
    }
    for (const auto length : lengths) {
        auto input = std::vector<std::byte>(length);
        for (auto& value : input) {
            value = static_cast<std::byte>(generator());
        }
        const auto ours = liquid::hash(liquid::algorithm::sha256, input);
        ASSERT_TRUE(ours) << length;
        const auto theirs = openssl(EVP_sha256(), input);
        ASSERT_EQ(theirs.size(), ours->bytes.size());
        EXPECT_TRUE(std::equal(theirs.begin(), theirs.end(), ours->bytes.begin(), [](unsigned char a, std::byte b) {
            return a == std::to_integer<unsigned char>(b);
        })) << "sha256 differs at length " << length;

        const auto sha1 = liquid::git_detail::sha1_hash(input);
        const auto expected = openssl(EVP_sha1(), input);
        ASSERT_EQ(expected.size(), sha1.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), sha1.begin(), [](unsigned char a, std::byte b) {
            return a == std::to_integer<unsigned char>(b);
        })) << "sha1 differs at length " << length;
    }
}
