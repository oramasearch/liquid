// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "protobuf/wire.hpp"

namespace {

TEST(ProtobufWire, covers_all_scalar_wire_forms_and_packed_values) {
    auto output = liquid::protobuf::writer{};
    output.varint(1, 300);
    output.sint32(2, -150);
    output.sint64(3, -9'000'000'000LL);
    output.fixed32(4, 0x78563412U);
    output.fixed64(5, 0xfedcba9876543210ULL);
    output.string(6, "wire");
    const auto packed = std::vector<std::uint64_t>{1, 128, 16'384};
    output.packed_varints(7, packed);
    auto bytes = std::move(output).take();

    auto input = liquid::protobuf::reader{bytes};
    auto first = input.next();
    ASSERT_TRUE(first);
    ASSERT_TRUE(*first);
    EXPECT_EQ((*first)->integer, 300U);
    auto second = input.next();
    ASSERT_TRUE(second);
    EXPECT_EQ(liquid::protobuf::zigzag_decode32(static_cast<std::uint32_t>((*second)->integer)),
              -150);
    auto third = input.next();
    ASSERT_TRUE(third);
    EXPECT_EQ(liquid::protobuf::zigzag_decode64((*third)->integer), -9'000'000'000LL);
    auto fourth = input.next();
    ASSERT_TRUE(fourth);
    EXPECT_EQ((*fourth)->integer, 0x78563412U);
    auto fifth = input.next();
    ASSERT_TRUE(fifth);
    EXPECT_EQ((*fifth)->integer, 0xfedcba9876543210ULL);
    auto sixth = input.next();
    ASSERT_TRUE(sixth);
    auto text = liquid::protobuf::require_string(**sixth, "test string");
    ASSERT_TRUE(text);
    EXPECT_EQ(*text, "wire");
    auto seventh = input.next();
    ASSERT_TRUE(seventh);
    auto unpacked = liquid::protobuf::unpack_varints(**seventh, "test packed field");
    ASSERT_TRUE(unpacked);
    EXPECT_EQ(*unpacked, packed);
    auto end = input.next();
    ASSERT_TRUE(end);
    EXPECT_FALSE(*end);
}

TEST(ProtobufWire, preserves_an_unknown_group_byte_for_byte) {
    const auto original = std::vector<std::byte>{
        std::byte{0x2b}, std::byte{0x08}, std::byte{0x96}, std::byte{0x01}, std::byte{0x2c}};
    auto input = liquid::protobuf::reader{original};
    auto decoded = input.next();
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(*decoded);
    EXPECT_EQ((*decoded)->type, liquid::protobuf::wire_type::start_group);
    auto retained = std::vector<std::byte>{};
    liquid::protobuf::preserve_unknown(retained, **decoded);
    EXPECT_EQ(retained, original);
}

TEST(ProtobufWire, rejects_truncated_and_invalid_fields) {
    const auto truncated =
        std::vector<std::byte>{std::byte{0x0a}, std::byte{0x02}, std::byte{0x01}};
    auto truncated_reader = liquid::protobuf::reader{truncated};
    EXPECT_FALSE(truncated_reader.next());

    const auto zero_field = std::vector<std::byte>{std::byte{0x00}};
    auto zero_reader = liquid::protobuf::reader{zero_field};
    EXPECT_FALSE(zero_reader.next());

    const auto oversized_key = std::vector<std::byte>{std::byte{0x88},
                                                      std::byte{0x80},
                                                      std::byte{0x80},
                                                      std::byte{0x80},
                                                      std::byte{0x10},
                                                      std::byte{0x00}};
    auto oversized_key_reader = liquid::protobuf::reader{oversized_key};
    EXPECT_FALSE(oversized_key_reader.next());
}

} // namespace
