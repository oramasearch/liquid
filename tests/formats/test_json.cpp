// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "formats/json.hpp"

TEST(JsonFormat, ParsesNestedValuesAndWritesStableReadableJson) {
    constexpr auto source = R"({"service":{"enabled":true,"ports":[8080,8443]},"token":"secret"})";
    auto parsed = liquid::formats::json::parse(source);
    ASSERT_TRUE(parsed) << parsed.error().message;

    auto written = liquid::formats::json::write(*parsed);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written,
              "{\n"
              "  \"service\": {\n"
              "    \"enabled\": true,\n"
              "    \"ports\": [\n"
              "      8080,\n"
              "      8443\n"
              "    ]\n"
              "  },\n"
              "  \"token\": \"secret\"\n"
              "}\n");

    auto reparsed = liquid::formats::json::parse(*written);
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    EXPECT_EQ(*reparsed, *parsed);
}

TEST(JsonFormat, PreservesObjectOrderNumberSpellingAndUnicode) {
    constexpr auto source = R"({"z":1.20e+3,"a":"\u00e9 \ud83d\ude80"})";
    auto parsed = liquid::formats::json::parse(source);
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto written = liquid::formats::json::write(*parsed);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, "{\n  \"z\": 1.20e+3,\n  \"a\": \"é 🚀\"\n}\n");
}

TEST(JsonFormat, RejectsInvalidAndAmbiguousDocuments) {
    EXPECT_FALSE(liquid::formats::json::parse(R"({"a":1,"a":2})"));
    EXPECT_FALSE(liquid::formats::json::parse(R"({"a":01})"));
    EXPECT_FALSE(liquid::formats::json::parse(R"([1,])"));
    EXPECT_FALSE(liquid::formats::json::parse(R"("\ud800")"));
    auto invalid_utf8 = std::string{"{\"a\":\""};
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0x80));
    invalid_utf8 += "\"}";
    EXPECT_FALSE(liquid::formats::json::parse(invalid_utf8));
}
