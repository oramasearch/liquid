#include <gtest/gtest.h>

#include "formats/dotenv.hpp"

TEST(DotenvFormat, PreservesCommentsBlankLinesAndAssignmentOrder) {
    constexpr auto source = "# service credentials\r\n\r\nTOKEN=secret\r\nexport PORT = 8080\r\n";
    auto parsed = liquid::formats::dotenv::parse(source);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->lines.size(), 4U);
    EXPECT_EQ(parsed->lines[0].kind, liquid::formats::dotenv::line_kind::comment);
    EXPECT_EQ(parsed->lines[2].key, "TOKEN");
    EXPECT_TRUE(parsed->lines[3].exported);

    auto written = liquid::formats::dotenv::write(*parsed);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, "# service credentials\n\nTOKEN=secret\nexport PORT= 8080\n");

    auto reparsed = liquid::formats::dotenv::parse(*written);
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    EXPECT_EQ(*reparsed, *parsed);
}

TEST(DotenvFormat, RejectsMissingAndDuplicateKeys) {
    EXPECT_FALSE(liquid::formats::dotenv::parse("not-an-assignment\n"));
    EXPECT_FALSE(liquid::formats::dotenv::parse("TOKEN=one\nTOKEN=two\n"));
    EXPECT_FALSE(liquid::formats::dotenv::parse("1TOKEN=value\n"));
}
