// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "scan/detectors.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto contains(const liquid::scan_detail::report& report, std::string_view detector) -> bool {
    return std::ranges::any_of(report.blocked,
                               [&](const auto& value) { return value.detector == detector; });
}

struct detector_case {
    std::string detector;
    std::string matching;
    std::string harmless;
};

class DetectorCases : public testing::TestWithParam<detector_case> {};

TEST_P(DetectorCases, distinguishes_matching_and_harmless_content) {
    const auto& parameter = GetParam();
    const auto scanner = liquid::scan_detail::detector_set{};
    const auto matching = bytes(parameter.matching);
    const auto harmless = bytes(parameter.harmless);
    const auto found = scanner.scan("config.txt", matching, {});
    const auto ignored = scanner.scan("config.txt", harmless, {});
    EXPECT_TRUE(contains(found, parameter.detector));
    EXPECT_FALSE(contains(ignored, parameter.detector));
}

INSTANTIATE_TEST_SUITE_P(
    SecretShapes,
    DetectorCases,
    testing::Values(
        detector_case{"aws-access-key",
                      "key=AKIA"
                      "0123456789ABCDEF",
                      "key=AKIB0123456789ABCDEF"},
        detector_case{"aws-secret-key",
                      "aws_secret_access_key=0123456789abcdefghijABCDEFGHIJ+/xyzwQRST",
                      "aws_secret_access_key=replace-me"},
        detector_case{"github-token",
                      "token=ghp_"
                      "0123456789abcdefghijABCDEFGHIJKLMNop",
                      "token=ghp_short"},
        detector_case{"slack-token", "token=xoxb-1234567890-abcdefghijklmno", "token=xoxb-short"},
        detector_case{"google-api-key",
                      "key=AIza"
                      "0123456789abcdefghijABCDEFGHIJ-_xyz",
                      "key=AIza-short"},
        detector_case{
            "private-key", "-----BEGIN OPENSSH PRIVATE KEY-----", "-----BEGIN PUBLIC KEY-----"},
        detector_case{
            "jwt", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTYifQ.abcdefghijk", "eyJ.not.a-token"},
        detector_case{"generic-assignment", "password=2jD8!qP4#vN7@xK9", "password=correct-horse"},
        detector_case{"dotenv-assignment",
                      "SERVICE_KEY=2jD8qP4vN7xK9mR5tY1wZ6cB",
                      "SERVICE_KEY=development"}),
    [](const testing::TestParamInfo<detector_case>& info) {
        return "Case" + std::to_string(info.index);
    });

TEST(DetectorConfiguration, records_allow_list_and_line_marker_reasons) {
    const auto scanner = liquid::scan_detail::detector_set{};
    const auto content = bytes("key=AKIA"
                               "0123456789ABCDEF\n"
                               "key=AIza"
                               "0123456789abcdefghijABCDEFGHIJ-_xyz liquid:allow-secret\n");
    liquid::scan_configuration config;
    config.allow.push_back({"fixtures/**", {"aws-access-key"}});
    const auto report = scanner.scan("fixtures/config.txt", content, config);
    EXPECT_TRUE(report.blocked.empty());
    ASSERT_GE(report.allowed.size(), 2U);
    EXPECT_TRUE(std::ranges::any_of(report.allowed, [](const auto& value) {
        return value.value.detector == "aws-access-key" && value.reason == "allow-list";
    }));
    EXPECT_TRUE(std::ranges::any_of(report.allowed, [](const auto& value) {
        return value.value.detector == "google-api-key" && value.reason == "line-marker";
    }));
}

TEST(DetectorConfiguration, changed_ranges_preserve_full_line_numbers) {
    const auto scanner = liquid::scan_detail::detector_set{};
    const auto content = bytes("ordinary\nkey=AKIA"
                               "0123456789ABCDEF\n");
    const auto first_line = std::array{liquid::scan_detail::byte_range{0, 9}};
    const auto skipped = scanner.scan("config.txt", content, {}, std::span{first_line});
    EXPECT_TRUE(skipped.blocked.empty());
    const auto second_line = std::array{liquid::scan_detail::byte_range{9, content.size() - 9}};
    const auto found = scanner.scan("config.txt", content, {}, std::span{second_line});
    ASSERT_FALSE(found.blocked.empty());
    EXPECT_EQ(found.blocked.front().line, 2U);
}

} // namespace
