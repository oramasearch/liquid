#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "liquid/store.hpp"

TEST(StoreLarge, RoundTrips256MiB) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("liquid-store-large-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    struct cleanup {
        std::filesystem::path path;
        ~cleanup() { std::filesystem::remove_all(path); }
    } remove{root};

    std::vector<std::byte> input(256ULL * 1024ULL * 1024ULL);
    std::uint64_t state = 0x123456789abcdef0ULL;
    for (auto& byte : input) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = std::byte{static_cast<unsigned char>(state)};
    }
    liquid::store_options options;
    options.index_cache_size = 16 * 1024 * 1024;
    auto store = liquid::store::open(root, options);
    ASSERT_TRUE(store.has_value()) << store.error().message;
    auto id = store->put(input);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    auto output = store->get(*id);
    ASSERT_TRUE(output.has_value()) << output.error().message;
    EXPECT_EQ(*output, input);
}
