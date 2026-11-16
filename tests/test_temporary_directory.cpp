// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "temporary_directory.hpp"

#if !defined(_WIN32)
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "socket_fixture.hpp"
#endif

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

#if !defined(_WIN32)

TEST(SocketFixture, short_roots_are_private_and_unique) {
    auto first = liquid::test::socket_fixture{"/tmp"};
    auto second = liquid::test::socket_fixture{"/tmp"};

    EXPECT_NE(first.path().parent_path(), second.path().parent_path());
    EXPECT_EQ(first.path().parent_path().parent_path(), "/tmp");
    EXPECT_TRUE(first.path().is_absolute());
    EXPECT_LT(first.path().native().size(), sizeof(sockaddr_un::sun_path));
    EXPECT_EQ(std::filesystem::status(first.path().parent_path()).permissions(),
              std::filesystem::perms::owner_all);
}

TEST(SocketFixture, long_and_relative_roots_fall_back_to_short_absolute_paths) {
    const auto deep_root =
        liquid::test::fixture_path(std::string(sizeof(sockaddr_un::sun_path), 'd'));
    std::filesystem::create_directory(deep_root);
    auto absolute = liquid::test::socket_fixture{deep_root};
    auto relative = liquid::test::socket_fixture{std::filesystem::relative(deep_root)};

    EXPECT_EQ(absolute.path().parent_path().parent_path(), "/tmp");
    EXPECT_EQ(relative.path().parent_path().parent_path(), "/tmp");
    EXPECT_LT(absolute.path().native().size(), sizeof(sockaddr_un::sun_path));
    EXPECT_LT(relative.path().native().size(), sizeof(sockaddr_un::sun_path));
    EXPECT_NE(absolute.path(), relative.path());
}

TEST(SocketFixture, scope_exit_removes_a_bound_socket_and_its_directory) {
    auto endpoint = std::filesystem::path{};
    {
        auto fixture = liquid::test::socket_fixture{};
        endpoint = fixture.path();
        auto address = sockaddr_un{};
        address.sun_family = AF_UNIX;
        const auto text = endpoint.string();
        ASSERT_LT(text.size(), sizeof(address.sun_path));
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(descriptor, 0);
        const auto bound =
            ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::close(descriptor);
        ASSERT_EQ(bound, 0);
        EXPECT_TRUE(std::filesystem::is_socket(endpoint));
    }
    EXPECT_FALSE(std::filesystem::exists(endpoint));
    EXPECT_FALSE(std::filesystem::exists(endpoint.parent_path()));
}

#endif
