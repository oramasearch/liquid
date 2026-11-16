// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "liquid/asn1.hpp"

namespace {

auto bytes(std::initializer_list<unsigned int> values) -> std::vector<std::byte> {
    std::vector<std::byte> output;
    output.reserve(values.size());
    for (const unsigned int value : values) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

} // namespace

TEST(Asn1, ReadsExactNestedDerSpans) {
    const auto encoded = bytes({0x30, 0x08, 0x02, 0x01, 0x2a, 0x04, 0x03, 0x61, 0x62, 0x63});
    liquid::asn1::reader input(encoded);
    const auto sequence = input.single();
    ASSERT_TRUE(sequence) << sequence.error().message;
    EXPECT_EQ(sequence->identifier,
              (liquid::asn1::tag{liquid::asn1::tag_class::universal, 16, true}));
    EXPECT_EQ(sequence->encoded.size(), encoded.size());

    liquid::asn1::reader children(sequence->content);
    const auto integer = children.next();
    ASSERT_TRUE(integer) << integer.error().message;
    ASSERT_TRUE(*integer);
    const auto integer_bytes = liquid::asn1::unsigned_integer(**integer);
    ASSERT_TRUE(integer_bytes) << integer_bytes.error().message;
    ASSERT_EQ(integer_bytes->size(), 1U);
    EXPECT_EQ(std::to_integer<unsigned int>(integer_bytes->front()), 42U);

    const auto octets = children.next();
    ASSERT_TRUE(octets) << octets.error().message;
    ASSERT_TRUE(*octets);
    EXPECT_EQ((*octets)->identifier,
              (liquid::asn1::tag{liquid::asn1::tag_class::universal, 4, false}));
    EXPECT_TRUE(children.empty());
}

TEST(Asn1, DecodesObjectIdentifiers) {
    const auto encoded = bytes({0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01});
    liquid::asn1::reader input(encoded);
    const auto value = input.single();
    ASSERT_TRUE(value) << value.error().message;
    const auto identifier = liquid::asn1::object_identifier(*value);
    ASSERT_TRUE(identifier) << identifier.error().message;
    EXPECT_EQ(*identifier, "2.16.840.1.101.3.4.2.1");
}

TEST(Asn1, RejectsBerAndNonCanonicalDer) {
    const std::array invalid = {
        bytes({0x30, 0x80, 0x00, 0x00}),
        bytes({0x04, 0x81, 0x01, 0x00}),
        bytes({0x04, 0x82, 0x00, 0x80}),
        bytes({0x1f, 0x1e, 0x00}),
        bytes({0x1f, 0x80, 0x1f, 0x00}),
        bytes({0x04, 0x02, 0x00}),
        bytes({0x05, 0x00, 0x00}),
    };
    for (const auto& encoded : invalid) {
        liquid::asn1::reader input(encoded);
        EXPECT_FALSE(input.single());
    }
}

TEST(Asn1, RejectsInvalidUnsignedIntegersAndIdentifiers) {
    for (const auto& encoded :
         {bytes({0x02, 0x00}), bytes({0x02, 0x01, 0xff}), bytes({0x02, 0x02, 0x00, 0x01})}) {
        liquid::asn1::reader input(encoded);
        const auto value = input.single();
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_FALSE(liquid::asn1::unsigned_integer(*value));
    }

    for (const auto& encoded :
         {bytes({0x06, 0x00}), bytes({0x06, 0x01, 0x80}), bytes({0x06, 0x02, 0x80, 0x00})}) {
        liquid::asn1::reader input(encoded);
        const auto value = input.single();
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_FALSE(liquid::asn1::object_identifier(*value));
    }
}
