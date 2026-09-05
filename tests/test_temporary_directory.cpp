#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "temporary_directory.hpp"

TEST(TemporaryDirectory, process_root_is_stable_and_uses_the_platform_temporary_root) {
    const auto& first = liquid::test::process_fixture_root();
    const auto& second = liquid::test::process_fixture_root();

    EXPECT_EQ(first, second);
    EXPECT_TRUE(
        std::filesystem::equivalent(first.parent_path(), std::filesystem::temp_directory_path()));
    EXPECT_TRUE(std::filesystem::is_directory(first));
    EXPECT_EQ(liquid::test::fixture_path("child").parent_path(), first);
}

TEST(TemporaryDirectory, scoped_roots_are_unique_and_removed_by_their_destructor) {
    const auto& parent = liquid::test::process_fixture_root();
    auto first_path = std::filesystem::path{};
    auto second_path = std::filesystem::path{};
    {
        auto first = liquid::test::detail::process_fixture_directory{parent};
        auto second = liquid::test::detail::process_fixture_directory{parent};
        first_path = first.path();
        second_path = second.path();
        EXPECT_NE(first_path, second_path);
        EXPECT_TRUE(std::filesystem::is_directory(first_path));
        EXPECT_TRUE(std::filesystem::is_directory(second_path));
    }
    EXPECT_FALSE(std::filesystem::exists(first_path));
    EXPECT_FALSE(std::filesystem::exists(second_path));
}

TEST(TemporaryDirectory, unusable_root_reports_the_writable_root_requirement) {
    const auto unusable_root = liquid::test::fixture_path("not-a-directory");
    {
        auto output = std::ofstream{unusable_root};
        ASSERT_TRUE(output.good());
    }

    try {
        auto directory = liquid::test::detail::process_fixture_directory{unusable_root};
        FAIL() << "expected fixture directory creation to fail";
    } catch (const std::runtime_error& error) {
        const auto message = std::string{error.what()};
        EXPECT_NE(message.find("writable temporary root"), std::string::npos);
        EXPECT_NE(message.find(unusable_root.string()), std::string::npos);
    }

    std::filesystem::remove(unusable_root);
}
