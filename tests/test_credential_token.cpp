// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "credential_token.hpp"

// L-30 (liquid#410): a token file is used only when its owner alone may read it, and the daemon's
// scrub removes the variable and blanks its bytes in the environment block.
TEST(CredentialToken, ATokenFileMustBeOwnerOnlyAndScrubBlanksTheEnvironment) {
    const auto directory = std::filesystem::temp_directory_path() / "liquid-token-410";
    std::filesystem::create_directories(directory);
    const auto file = directory / "token";
    {
        auto output = std::ofstream{file, std::ios::trunc};
        output << "lake-token-value\n";
    }
    using std::filesystem::perms;
    std::filesystem::permissions(file, perms::owner_read | perms::owner_write);
    EXPECT_EQ(liquid::token_detail::read_token_file(file), std::optional<std::string>{"lake-token-value"});
    std::filesystem::permissions(file, perms::owner_read | perms::group_read);
    EXPECT_FALSE(liquid::token_detail::read_token_file(file).has_value());
    std::filesystem::remove_all(directory);

    ::setenv("LIQUID_TOKEN", "scrub-me", 1);
    ASSERT_NE(std::getenv("LIQUID_TOKEN"), nullptr);
    liquid::token_detail::scrub_environment_token();
    EXPECT_EQ(std::getenv("LIQUID_TOKEN"), nullptr) << "the daemon's children no longer inherit it";
}
