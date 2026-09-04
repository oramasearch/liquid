#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "objects/records.hpp"

namespace {

auto from_hex(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    for (std::size_t index = 0; index + 1 < input.size(); index += 2) {
        const auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
        output.push_back(std::byte{
            static_cast<unsigned char>((digit(input[index]) << 4) | digit(input[index + 1]))});
    }
    return output;
}

TEST(Records, empty_tree_fixture_is_canonical) {
    auto input =
        std::ifstream{std::string{LIQUID_SOURCE_DIR} + "/tests/fixtures/records/empty-tree.hex"};
    ASSERT_TRUE(input);
    auto hex = std::string{};
    input >> hex;
    auto fixture = from_hex(hex);
    auto decoded = liquid::objects::decode_tree(fixture);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(decoded->entries.empty());
    auto encoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, fixture);
    auto digest = liquid::objects::digest_of(liquid::algorithm::blake3, *encoded);
    ASSERT_TRUE(digest);
    EXPECT_EQ(digest->alg, liquid::algorithm::blake3);
}

TEST(Records, blob_records_carry_range_addressable_chunk_sizes) {
    auto chunk = liquid::digest{liquid::algorithm::blake3, {}};
    chunk.bytes.front() = std::byte{0x42};
    const auto original = liquid::blob{{chunk}, 42, {42}};
    auto encoded = liquid::objects::encode(original);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_blob(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded->chunk_sizes.size(), 1U);
    EXPECT_EQ(decoded->chunk_sizes.front(), 42U);

    const auto invalid = liquid::blob{{chunk}, 42, {41}};
    auto refused = liquid::objects::encode(invalid);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
}

} // namespace
