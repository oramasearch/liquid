// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <boost/capy/test/run_blocking.hpp>
#include <pthread.h>
#include <sys/mman.h>
#endif

#include "index.hpp"
#include "memory_backend.hpp"
#include "temporary_directory.hpp"
#include "write_once_backend.hpp"

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

// The disk backend at `root`, or the write-once backend over it when the suite runs a second time
// under liquid::sim::write_once_environment (liquid #332).
std::shared_ptr<liquid::store_backend> index_backend(const std::filesystem::path& root) {
    if (auto backend = liquid::sim::test_store_backend(root)) {
        return backend;
    }
    return liquid::make_disk_store_backend(root);
}

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

task<void> check_all(chunk_index& index, std::span<const index_entry> entries) {
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
        chunk_index index(index_backend(directory.path()), "", directory.path());
        ASSERT_TRUE(block_on(index.open()).has_value());
        ASSERT_TRUE(block_on(insert_all(index, entries)).has_value());
        ASSERT_TRUE(block_on(index.close()).has_value());
    }
    reset_file_read_counter();
    chunk_index reopened(index_backend(directory.path()), "", directory.path());
    ASSERT_TRUE(block_on(reopened.open()).has_value());
    // Only a mapped run reads just its header and bloom; the write-once backend hides the local
    // root, so its runs load into memory as a remote store's do.
    if (!liquid::sim::write_once_requested()) {
        EXPECT_LT(file_read_counter(), 2U * 1024U * 1024U);
    }
    ASSERT_TRUE(find_present(reopened, entries.back().id).has_value());
    ASSERT_TRUE(block_on(reopened.close()).has_value());
}

TEST(ChunkIndex, CompactionPreservesRandomKeysAndMisses) {
    temporary_directory directory;
    chunk_index index(
        index_backend(directory.path()), "", directory.path(), chunk_index_options{32, 2});
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
    chunk_index index(
        index_backend(directory.path()), "", directory.path(), chunk_index_options{16, 2});
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
    chunk_index reopened(
        index_backend(directory.path()), "", directory.path(), chunk_index_options{16, 2});
    ASSERT_TRUE(block_on(reopened.open()).has_value());
    ASSERT_TRUE(block_on(check_all(reopened, entries)).has_value());
    ASSERT_TRUE(block_on(reopened.close()).has_value());
}

// The write-once backend has the range contract of Zetta's object storage (liquid #332): it appends
// at the object's end, accepts a retry of bytes it already holds, and refuses anything else.
TEST(WriteOnceBackend, AppendsAndRefusesRewritesAndGaps) {
    auto inner = std::make_shared<liquid::sim::memory_store_backend>();
    liquid::sim::write_once_store_backend backend(inner);
    const auto one = std::vector<std::byte>{std::byte{1}, std::byte{2}};
    const auto two = std::vector<std::byte>{std::byte{3}};
    ASSERT_TRUE(block_on(backend.put_range("object", 0, one)));
    ASSERT_TRUE(block_on(backend.put_range("object", 2, two)));
    EXPECT_TRUE(block_on(backend.put_range("object", 1, {std::byte{2}})));
    auto rewrite = block_on(backend.put_range("object", 0, {std::byte{9}}));
    ASSERT_FALSE(rewrite);
    EXPECT_EQ(rewrite.error().code, liquid::errc::conflict);
    auto gap = block_on(backend.put_range("object", 4, two));
    ASSERT_FALSE(gap);
    EXPECT_EQ(gap.error().code, liquid::errc::conflict);
    auto absent = block_on(backend.put_range("other", 1, two));
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, liquid::errc::conflict);
    EXPECT_EQ(backend.refused(), 3U);
    auto held = block_on(backend.get_range("object", 0, 3));
    ASSERT_TRUE(held);
    EXPECT_EQ(*held, (std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}));
    ASSERT_TRUE(block_on(backend.put("object", two)));
    EXPECT_EQ(block_on(backend.get_range("object", 0, 1)).value_or(std::vector<std::byte>{}), two);
}

// Fails one put_range to one key, after `skip` earlier ones to that key have succeeded.
class failing_range_backend final : public liquid::store_backend {
public:
    failing_range_backend(std::shared_ptr<liquid::store_backend> inner,
                          std::string key,
                          unsigned skip)
        : inner_(std::move(inner)), key_(std::move(key)), skip_(skip) {}

    task<void> put(std::string key, std::vector<std::byte> bytes) override {
        return inner_->put(std::move(key), std::move(bytes));
    }
    task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override {
        if (armed_ && key == key_ && seen_++ == skip_) {
            armed_ = false;
            return failed(std::move(key));
        }
        return inner_->put_range(std::move(key), offset, std::move(bytes));
    }
    task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override {
        return inner_->get_range(std::move(key), offset, size);
    }
    task<void> remove(std::string key) override {
        return inner_->remove(std::move(key));
    }
    task<std::vector<liquid::store_backend_entry>> list(std::string prefix) override {
        return inner_->list(std::move(prefix));
    }
    task<void> barrier() override {
        return inner_->barrier();
    }

private:
    static task<void> failed(std::string key) {
        co_return std::unexpected(liquid::error{liquid::errc::unavailable, "injected", key});
    }

    std::shared_ptr<liquid::store_backend> inner_;
    std::string key_;
    unsigned skip_;
    unsigned seen_{};
    bool armed_{true};
};

task<void> flush_batches(chunk_index& index, std::span<const index_entry> entries) {
    for (std::size_t batch = 0; batch < entries.size(); batch += 16) {
        for (const auto& entry :
             entries.subspan(batch, std::min<std::size_t>(16, entries.size() - batch))) {
            auto inserted = index.insert(entry);
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
}

// Flush and compaction write every run once, in order, so a backend that refuses rewrites refuses
// nothing (liquid #332). The merge's header used to be written as a placeholder and rewritten.
TEST(ChunkIndex, CompactionWritesEachRunOnce) {
    auto memory = std::make_shared<liquid::sim::memory_store_backend>();
    auto backend = std::make_shared<liquid::sim::write_once_store_backend>(memory);
    std::vector<index_entry> entries;
    std::mt19937_64 random(332);
    for (std::uint64_t i = 0; i != 512; ++i) {
        entries.push_back({make_digest(random()), {static_cast<std::uint32_t>(i), i * 29, 61}});
    }
    {
        chunk_index index(backend, "index", "memory", chunk_index_options{16, 2});
        ASSERT_TRUE(block_on(index.open()));
        auto written = block_on(flush_batches(index, entries));
        ASSERT_TRUE(written) << written.error().message;
        ASSERT_TRUE(block_on(check_all(index, entries)));
        ASSERT_TRUE(block_on(index.close()));
    }
    EXPECT_EQ(backend->refused(), 0U);
    auto listed = block_on(memory->list("index/run-"));
    ASSERT_TRUE(listed);
    // 32 flushes leave far fewer runs, so compaction ran.
    EXPECT_LT(listed->size(), 8U);
    chunk_index reopened(backend, "index", "memory", chunk_index_options{16, 2});
    ASSERT_TRUE(block_on(reopened.open()));
    ASSERT_TRUE(block_on(check_all(reopened, entries)));
    ASSERT_TRUE(block_on(reopened.close()));
}

// A merge torn after its run's header and bloom are written leaves the manifest naming the old
// runs, which still answer every lookup. The next merge takes the same unpublished key and
// replaces the torn object whole, which a write-once backend allows.
TEST(ChunkIndex, TornMergeLeavesOldRunsAuthoritative) {
    auto memory = std::make_shared<liquid::sim::memory_store_backend>();
    auto write_once = std::make_shared<liquid::sim::write_once_store_backend>(memory);
    std::vector<index_entry> entries;
    for (std::uint64_t i = 0; i != 96; ++i) {
        entries.push_back({make_digest(i * 104729), {static_cast<std::uint32_t>(i), i * 13, 47}});
    }
    const auto first = std::span<const index_entry>(entries).first(32);
    const auto third = std::span<const index_entry>(entries).subspan(32, 16);
    {
        chunk_index index(write_once, "index", "memory", chunk_index_options{16, 2});
        ASSERT_TRUE(block_on(index.open()));
        ASSERT_TRUE(block_on(flush_batches(index, first)));
    }
    {
        // The third flush writes run 3 and merges runs 1-3 into run 4. The second range written
        // to run 4, its records after the bloom, fails.
        auto failing = std::make_shared<failing_range_backend>(write_once, "index/run-4.idx", 1);
        chunk_index index(failing, "index", "memory", chunk_index_options{16, 2});
        ASSERT_TRUE(block_on(index.open()));
        auto torn = block_on(flush_batches(index, third));
        ASSERT_FALSE(torn);
        EXPECT_EQ(torn.error().code, liquid::errc::unavailable);
    }
    auto runs = block_on(memory->list("index/run-4"));
    ASSERT_TRUE(runs);
    ASSERT_EQ(runs->size(), 1U);
    {
        chunk_index index(write_once, "index", "memory", chunk_index_options{16, 2});
        ASSERT_TRUE(block_on(index.open()));
        ASSERT_TRUE(block_on(check_all(index, first)));
        ASSERT_TRUE(
            block_on(flush_batches(index, std::span<const index_entry>(entries).subspan(32))));
        ASSERT_TRUE(block_on(index.close()));
    }
    chunk_index reopened(write_once, "index", "memory", chunk_index_options{16, 2});
    ASSERT_TRUE(block_on(reopened.open()));
    ASSERT_TRUE(block_on(check_all(reopened, entries)));
    EXPECT_EQ(write_once->refused(), 0U);
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
        std::shared_ptr<liquid::store_backend> backend = job.backend;
        if (liquid::sim::write_once_requested()) {
            backend = std::make_shared<liquid::sim::write_once_store_backend>(job.backend);
        }
        chunk_index index(backend, "index", "memory", chunk_index_options{16, 2});
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
