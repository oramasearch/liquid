#include "encoding/cbor.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace cbor = liquid::encoding::cbor;

auto hex(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    for (std::size_t i = 0; i < input.size(); i += 2) {
        output.push_back(std::byte{
            static_cast<unsigned char>(std::stoul(std::string{input.substr(i, 2)}, nullptr, 16))});
    }
    return output;
}

auto encode_checked(const cbor::value &value) -> std::vector<std::byte> {
    auto result = cbor::encode(value);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(std::vector<std::byte>{});
}

TEST(Cbor, rfc_8949_supported_vectors) {
    const auto path =
        std::filesystem::path{LIQUID_SOURCE_DIR} / "tests/fixtures/cbor/rfc8949-supported.txt";
    auto fixture = std::ifstream{path};
    ASSERT_TRUE(fixture);
    auto lines = std::size_t{0};
    for (auto line = std::string{}; std::getline(fixture, line);) {
        if (line.empty() || line.front() == '#')
            continue;
        const auto separator = line.find('|');
        ASSERT_NE(separator, std::string::npos);
        const auto notation = line.substr(0, separator);
        const auto expected = hex(line.substr(separator + 1));
        cbor::value input;
        if (notation == "false")
            input = cbor::value{false};
        else if (notation == "true")
            input = cbor::value{true};
        else if (notation == "null")
            input = cbor::value{cbor::null{}};
        else if (notation.starts_with("bytes:"))
            input = cbor::value{hex(notation.substr(6))};
        else if (notation.starts_with("text:"))
            input = cbor::value{notation.substr(5)};
        else if (notation == "array:")
            input = cbor::value{cbor::value::array{}};
        else if (notation == "array:1,2,3")
            input = cbor::value{cbor::value::array{cbor::value{std::uint64_t{1}},
                                                   cbor::value{std::uint64_t{2}},
                                                   cbor::value{std::uint64_t{3}}}};
        else if (notation == "map:a=1,b=2")
            input =
                cbor::value{cbor::value::map{{cbor::value{"a"}, cbor::value{std::uint64_t{1}}},
                                             {cbor::value{"b"}, cbor::value{std::uint64_t{2}}}}};
        else if (notation.front() == '-')
            input = cbor::value{static_cast<std::int64_t>(std::stoll(notation))};
        else
            input = cbor::value{static_cast<std::uint64_t>(std::stoull(notation))};

        EXPECT_EQ(encode_checked(input), expected) << notation;
        auto decoded = cbor::decode(expected);
        ASSERT_TRUE(decoded.has_value()) << notation;
        EXPECT_EQ(encode_checked(*decoded), expected) << notation;
        ++lines;
    }
    EXPECT_EQ(lines, 25);
}

TEST(Cbor, map_input_is_sorted_by_encoded_key) {
    auto input = cbor::value{cbor::value::map{
        {cbor::value{"b"}, cbor::value{std::uint64_t{2}}},
        {cbor::value{"a"}, cbor::value{std::uint64_t{1}}},
    }};
    EXPECT_EQ(encode_checked(input), hex("a2616101616202"));
}

TEST(Cbor, rejects_noncanonical_and_unsupported_input) {
    EXPECT_FALSE(cbor::decode(hex("1817")));
    EXPECT_FALSE(cbor::decode(hex("9f01ff")));
    EXPECT_FALSE(cbor::decode(hex("f93c00")));
    EXPECT_FALSE(cbor::decode(hex("a2616202616101")));
    EXPECT_FALSE(cbor::decode(hex("0000")));
    EXPECT_FALSE(cbor::decode(hex("61ff")));
}

} // namespace
