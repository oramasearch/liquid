#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

#include "index.hpp"
#include "temporary_directory.hpp"

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
    temporary_directory()
        : path_(liquid::test::fixture_path("liquid-index-test-" + std::to_string(++next_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_directory() {
        std::filesystem::remove_all(path_);
    }
    const std::filesystem::path& path() const {
        return path_;
    }

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
        auto inserted = index.insert(entry);
        if (!inserted)
            co_return std::unexpected(inserted.error());
        if (index.needs_flush(entry.id.alg)) {
            auto flushed = co_await index.flush();
            if (!flushed)
                co_return std::unexpected(flushed.error());
        }
    }
    co_return liquid::result<void>{};
}

task<void> check_all(chunk_index& index, const std::vector<index_entry>& entries) {
    for (const auto& entry : entries) {
        auto found = index.find(entry.id);
        if (!found)
            co_return std::unexpected(found.error());
        if (!*found || (**found).pack_id != entry.location.pack_id)
            co_return std::unexpected(
                liquid::error{liquid::errc::corrupt, "index lookup mismatch", {}});
    }
    co_return liquid::result<void>{};
}

liquid::result<bool> find_present(chunk_index& index, digest id) {
    auto found = index.find(id);
    if (!found)
        return std::unexpected(found.error());
    return found->has_value();
}

TEST(ChunkIndex, ReopenReadsHeadersAndBloomOnly) {
    temporary_directory directory;
    std::vector<index_entry> entries;
    entries.reserve(1'000'000);
    for (std::uint64_t i = 0; i != 1'000'000; ++i)
        entries.push_back(
            {make_digest(i), {static_cast<std::uint32_t>(i / 1024), i * 48, 64 * 1024}});
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
    ASSERT_TRUE(find_present(reopened, entries.back().id).has_value());
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
    auto missing = find_present(index, make_digest(0xfeedbeefULL));
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
    auto sha = make_digest(44, algorithm::sha256);
    ASSERT_TRUE(index.insert({sha, {9, 10, 11}}).has_value());
    auto sha_found = find_present(index, sha);
    ASSERT_TRUE(sha_found.has_value());
    EXPECT_TRUE(*sha_found);
    ASSERT_TRUE(block_on(index.close()).has_value());
}

// Repeated flushes drive compaction while `state.runs` grows. The merge must not depend on the
// vector's storage staying put: it used to hold raw pointers into it across every write (issue
// #271). This test covers the interleaving; it does not by itself reproduce the multi-threaded
// crash lake saw.
TEST(ChunkIndex, CompactionSurvivesAConcurrentFlush) {
    temporary_directory directory;
    chunk_index index(directory.path(), chunk_index_options{16, 2});
    ASSERT_TRUE(block_on(index.open()).has_value());
    std::vector<index_entry> entries;
    std::mt19937_64 random(1471);
    for (std::uint64_t i = 0; i != 512; ++i) {
        auto id = make_digest(random());
        entries.push_back({id, {static_cast<std::uint32_t>(i), i * 23, 71}});
    }
    // Every flush appends a run and triggers compaction; interleaving inserts with flushes drives
    // the merge while the vector behind it grows.
    ASSERT_TRUE(block_on([&]() -> task<void> {
                    for (std::size_t batch = 0; batch != entries.size(); batch += 16) {
                        const auto last = std::min(batch + 16, entries.size());
                        for (std::size_t i = batch; i != last; ++i) {
                            auto inserted = index.insert(entries[i]);
                            if (!inserted) {
                                co_return std::unexpected(inserted.error());
                            }
                        }
                        auto flushed = co_await index.flush();
                        if (!flushed) {
                            co_return std::unexpected(flushed.error());
                        }
                    }
                    co_return liquid::result<void>{};
                }())
                    .has_value());
    ASSERT_TRUE(block_on(check_all(index, entries)).has_value());
    ASSERT_TRUE(block_on(index.close()).has_value());
    chunk_index reopened(directory.path(), chunk_index_options{16, 2});
    ASSERT_TRUE(block_on(reopened.open()).has_value());
    ASSERT_TRUE(block_on(check_all(reopened, entries)).has_value());
    ASSERT_TRUE(block_on(reopened.close()).has_value());
}

} // namespace
