#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

#include "index.hpp"

namespace {

using liquid::algorithm;
using liquid::digest;
using liquid::runtime::block_on;
using liquid::runtime::task;
using liquid::store_detail::chunk_index;
using liquid::store_detail::chunk_index_options;
using liquid::store_detail::chunk_location;
using liquid::store_detail::file_read_counter;
using liquid::store_detail::index_entry;
using liquid::store_detail::reset_file_read_counter;

class temporary_directory {
public:
    temporary_directory() : path_(std::filesystem::temp_directory_path() /
                                  ("liquid-index-test-" + std::to_string(++next_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_directory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }
private:
    inline static unsigned next_{};
    std::filesystem::path path_;
};

digest make_digest(std::uint64_t value, algorithm alg = algorithm::blake3) {
    digest output{alg, {}};
    for (unsigned byte = 0; byte < 8; ++byte)
        output.bytes[31 - byte] = std::byte{static_cast<unsigned char>(value >> (byte * 8U))};
    return output;
}

task<void> insert_all(chunk_index& index, const std::vector<index_entry>& entries) {
    for (const auto& entry : entries) {
        auto inserted = co_await index.insert(entry);
        if (!inserted) co_return std::unexpected(inserted.error());
    }
    co_return liquid::result<void>{};
}

task<void> check_all(chunk_index& index, const std::vector<index_entry>& entries) {
    for (const auto& entry : entries) {
        auto found = co_await index.find(entry.id);
        if (!found) co_return std::unexpected(found.error());
        if (!*found || (**found).pack_id != entry.location.pack_id)
            co_return std::unexpected(liquid::error{liquid::errc::corrupt, "index lookup mismatch", {}});
    }
    co_return liquid::result<void>{};
}

TEST(ChunkIndex, ReopenReadsHeadersAndBloomOnly) {
    temporary_directory directory;
    std::vector<index_entry> entries;
    entries.reserve(1'000'000);
    for (std::uint64_t i = 0; i != 1'000'000; ++i)
        entries.push_back({make_digest(i), {static_cast<std::uint32_t>(i / 1024), i * 48, 64 * 1024}});
    {
        chunk_index index(directory.path());
        ASSERT_TRUE(block_on(index.open()).has_value());
        ASSERT_TRUE(block_on(insert_all(index, entries)).has_value());
        ASSERT_TRUE(block_on(index.close()).has_value());
    }
    reset_file_read_counter();
    chunk_index reopened(directory.path());
    ASSERT_TRUE(block_on(reopened.open()).has_value());
    EXPECT_LT(file_read_counter(), 2U * 1024U * 1024U);
    ASSERT_TRUE(block_on(reopened.find(entries.back().id)).has_value());
    ASSERT_TRUE(block_on(reopened.close()).has_value());
}

TEST(ChunkIndex, CompactionPreservesRandomKeysAndMisses) {
    temporary_directory directory;
    chunk_index index(directory.path(), chunk_index_options{32, 2});
    ASSERT_TRUE(block_on(index.open()).has_value());
    std::vector<index_entry> entries;
    std::mt19937_64 random(71);
    for (std::uint64_t i = 0; i != 320; ++i) {
        auto id = make_digest(random());
        entries.push_back({id, {static_cast<std::uint32_t>(i), i * 17, 99}});
    }
    ASSERT_TRUE(block_on(insert_all(index, entries)).has_value());
    ASSERT_TRUE(block_on(index.flush()).has_value());
    ASSERT_TRUE(block_on(check_all(index, entries)).has_value());
    auto missing = block_on(index.find(make_digest(0xfeedbeefULL)));
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
    auto sha = make_digest(44, algorithm::sha256);
    ASSERT_TRUE(block_on(index.insert({sha, {9, 10, 11}})).has_value());
    auto sha_found = block_on(index.find(sha));
    ASSERT_TRUE(sha_found.has_value());
    EXPECT_TRUE(*sha_found);
    ASSERT_TRUE(block_on(index.close()).has_value());
}

}  // namespace
