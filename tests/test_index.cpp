#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

#if !defined(_WIN32)
#include <boost/capy/test/run_blocking.hpp>
#include <pthread.h>
#include <sys/mman.h>
#endif

#include "index.hpp"
#include "memory_backend.hpp"
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

#if !defined(_WIN32)
// Run `coroutine` to completion on the calling thread. `block_on` spreads work over runner threads,
// so it cannot pin a coroutine ramp to the thread whose stack is under test.
liquid::result<void> run_here(task<void> coroutine) {
    liquid::result<void> output =
        std::unexpected(liquid::error{liquid::errc::internal, "not run", {}});
    boost::capy::test::run_blocking([&](liquid::result<void> value) { output = std::move(value); })(
        std::move(coroutine));
    return output;
}

task<void> flush_and_compact(chunk_index& index, std::vector<index_entry> entries) {
    auto opened = co_await index.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto inserted = co_await insert_all(index, entries);
    if (!inserted) {
        co_return std::unexpected(inserted.error());
    }
    auto flushed = co_await index.flush();
    if (!flushed) {
        co_return std::unexpected(flushed.error());
    }
    auto checked = co_await check_all(index, entries);
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    co_return co_await index.close();
}

// Embedders run the index on secondary threads, which get 512 KiB stacks on macOS. The flush and
// compaction coroutines once held two 65,537-entry fanout arrays, and at -O0 their ramps reserved
// stack for them: 256 KiB for flush and 516 KiB for compaction (issue #323). This thread gets a
// 512 KiB stack above 1 MiB of inaccessible memory, so a ramp that reaches past it faults every
// time instead of landing in whatever happens to be mapped below. The in-memory backend completes
// inline, so every ramp runs on this thread.
TEST(ChunkIndex, CompactionRunsOnASmallThreadStack) {
    constexpr std::size_t stack_size = 512 * 1024;
    constexpr std::size_t guard_size = 1024 * 1024;
    auto* const region = static_cast<std::byte*>(
        ::mmap(nullptr, guard_size + stack_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    ASSERT_NE(region, MAP_FAILED);
    ASSERT_EQ(::mprotect(region + guard_size, stack_size, PROT_READ | PROT_WRITE), 0);

    struct work {
        std::shared_ptr<liquid::sim::memory_store_backend> backend =
            std::make_shared<liquid::sim::memory_store_backend>();
        std::size_t runs = 0;
        bool passed = false;
    } job;
    const auto body = [](void* argument) -> void* {
        auto& job = *static_cast<work*>(argument);
        chunk_index index(job.backend, "index", "memory", chunk_index_options{16, 2});
        std::vector<index_entry> entries;
        for (std::uint64_t i = 0; i != 64; ++i) {
            entries.push_back({make_digest(i * 7919), {static_cast<std::uint32_t>(i), i * 31, 57}});
        }
        job.passed = run_here(flush_and_compact(index, std::move(entries))).has_value();
        boost::capy::test::run_blocking(
            [&](liquid::result<std::vector<liquid::store_backend_entry>> listed) {
                for (const auto& entry :
                     listed.value_or(std::vector<liquid::store_backend_entry>{})) {
                    job.runs += entry.key.find("run-") != std::string::npos ? 1 : 0;
                }
            })(job.backend->list("index/"));
        return nullptr;
    };
    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstack(&attributes, region + guard_size, stack_size), 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, &attributes, body, &job), 0);
    pthread_attr_destroy(&attributes);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
    ::munmap(region, guard_size + stack_size);
    EXPECT_TRUE(job.passed);
    // Four flushes of 16 entries: three runs in one tier merge into one, leaving it and the fourth.
    EXPECT_EQ(job.runs, 2U);
}
#endif

} // namespace
