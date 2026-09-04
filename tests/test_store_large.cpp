#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

TEST(StoreLarge, RoundTrips256MiB) {
    const auto path = std::filesystem::temp_directory_path() / "liquid-store-large";
    std::filesystem::remove_all(path);
    std::vector<std::byte> source(256U * 1024U * 1024U);
    std::mt19937_64 random(22);
    for (auto& byte : source) byte = std::byte{static_cast<unsigned char>(random())};
    auto opened = liquid::runtime::block_on(liquid::store::open(path));
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    auto id = liquid::runtime::block_on(opened->put(source));
    ASSERT_TRUE(id.has_value()) << id.error().message;
    auto got = liquid::runtime::block_on(opened->get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
    std::filesystem::remove_all(path);
}
