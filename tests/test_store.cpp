#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

class temporary_store {
public:
    temporary_store() : path_(std::filesystem::temp_directory_path() /
                              ("liquid-store-test-" + std::to_string(++counter_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_store() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned counter_{};
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::size_t size, std::uint64_t seed = 9) {
    std::vector<std::byte> output(size);
    std::mt19937_64 random(seed);
    for (auto& byte : output) byte = std::byte{static_cast<unsigned char>(random())};
    return output;
}

liquid::store open_store(const std::filesystem::path& path) {
    auto opened = liquid::runtime::block_on(liquid::store::open(path));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

TEST(Store, ChunkingProducesStableObjectIdentity) {
    const auto source = bytes(4U * 1024U * 1024U, 4);
    temporary_store first_path;
    temporary_store second_path;
    auto first_store = open_store(first_path.path());
    auto second_store = open_store(second_path.path());
    const auto first = liquid::runtime::block_on(first_store.put(source));
    const auto second = liquid::runtime::block_on(second_store.put(source));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
}

TEST(Store, PutGetReadDeduplicateAndReopen) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(10U * 1024U * 1024U, 7);
    auto first = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    auto got = liquid::runtime::block_on(store.get(*first));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);

    std::vector<std::byte> range(8192);
    auto read = liquid::runtime::block_on(store.read(*first, 65536 - 100, range));
    ASSERT_TRUE(read.has_value()) << read.error().message;
    EXPECT_EQ(*read, range.size());
    EXPECT_TRUE(std::equal(range.begin(), range.end(), source.begin() + 65536 - 100));
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    const auto before = std::filesystem::file_size(pack);
    auto second = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
    EXPECT_EQ(before, std::filesystem::file_size(pack));
    store = {};
    auto reopened = open_store(temporary.path());
    got = liquid::runtime::block_on(reopened.get(*first));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

TEST(Store, RebuildsIndexAndReportsPackCorruption) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(256U * 1024U, 12);
    auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    auto rebuilt = open_store(temporary.path());
    auto got = liquid::runtime::block_on(rebuilt.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
    rebuilt = {};

    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    std::fstream damaged(pack, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(damaged.good());
    damaged.seekg(20);
    char changed = 0;
    damaged.read(&changed, 1);
    damaged.clear();
    damaged.seekp(20);
    changed ^= 0x7f;
    damaged.write(&changed, 1);
    damaged.close();
    auto corrupt = open_store(temporary.path());
    auto failed = liquid::runtime::block_on(corrupt.get(*id));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, liquid::errc::corrupt);
}

TEST(Store, RefusesUnknownFormatAndRebuildsBadRun) {
    temporary_store unknown;
    std::filesystem::create_directories(unknown.path());
    { std::ofstream marker(unknown.path() / "LIQUID_STORE"); marker << "liquid-store 99\n"; }
    auto bad = liquid::runtime::block_on(liquid::store::open(unknown.path()));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, liquid::errc::unsupported);

    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(4096, 3);
    auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    store = {};
    const auto run = temporary.path() / "index" / "run-1.idx";
    ASSERT_TRUE(std::filesystem::exists(run));
    std::fstream broken(run, std::ios::in | std::ios::out | std::ios::binary);
    broken.seekp(0); broken.put('X'); broken.close();
    auto reopened = open_store(temporary.path());
    auto got = liquid::runtime::block_on(reopened.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

}  // namespace
