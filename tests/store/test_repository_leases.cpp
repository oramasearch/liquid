// SPDX-License-Identifier: MIT

// liquid #527: a repository handle's own leases (its checkpoint, its directories) carry the
// process that holds them, are released when the handle ends, and are collected by the next open
// when their process has ended. A checkout opened and closed a thousand times keeps no more leases
// than its live handles hold.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "store/backend.hpp"
#include "store/holder.hpp"
#include "store/leases.hpp"

#ifndef _WIN32
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path fresh(const std::string& name) {
    const auto path = liquid::test::fixture_path(name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

std::size_t lease_files(const std::filesystem::path& checkout) {
    std::size_t count = 0;
    const auto directory = checkout / ".liquid-store" / "leases";
    if (!std::filesystem::exists(directory))
        return 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".lease")
            ++count;
    }
    return count;
}

std::vector<liquid::store_detail::lease_record>
lease_records(const std::filesystem::path& checkout) {
    const auto root = checkout / ".liquid-store";
    auto records = liquid::runtime::block_on(
        liquid::store_detail::load_leases(liquid::make_disk_store_backend(root), root / "leases"));
    EXPECT_TRUE(records) << records.error().message;
    return records ? *records : std::vector<liquid::store_detail::lease_record>{};
}

std::filesystem::path checkout_with_three_files(const std::string& name) {
    const auto checkout = fresh(name) / "checkout";
    auto created = liquid::runtime::block_on(liquid::repository::create(checkout));
    EXPECT_TRUE(created) << created.error().message;
    for (const auto* path : {"a.txt", "b.txt", "notes/c.txt"}) {
        auto written =
            liquid::runtime::block_on(created->write(path, bytes_of(std::string{path} + "\n")));
        EXPECT_TRUE(written) << written.error().message;
    }
    return checkout;
}

TEST(RepositoryLeases, TheHolderLineNamesThisProcessAndItIsAlive) {
    const auto holder = liquid::store_detail::process_holder();
    EXPECT_TRUE(holder.starts_with("holder ")) << holder;
    EXPECT_EQ(liquid::store_detail::lease_holder_state("repository:00\ncheckpoint\n" + holder),
              liquid::store_detail::holder_state::alive);
    EXPECT_EQ(liquid::store_detail::lease_holder_state("repository:00\ncheckpoint"),
              liquid::store_detail::holder_state::unknown);
    EXPECT_EQ(liquid::store_detail::lease_holder_state(
                  "repository:00\ncheckpoint\nholder another-host.invalid 12 34"),
              liquid::store_detail::holder_state::unknown);
}

TEST(RepositoryLeases, AHandleReleasesItsLeasesWhenItEnds) {
    const auto checkout = checkout_with_three_files("repository-leases-release");
    EXPECT_EQ(lease_files(checkout), 0U) << "the creating handle released its leases";
    {
        auto opened = liquid::runtime::block_on(liquid::repository::open(checkout));
        ASSERT_TRUE(opened) << opened.error().message;
        const auto held = lease_files(checkout);
        EXPECT_GT(held, 0U) << "an open handle leases its checkpoint";
        auto counted = liquid::runtime::block_on(opened->lease_count());
        ASSERT_TRUE(counted) << counted.error().message;
        EXPECT_EQ(*counted, held);
        for (const auto& record : lease_records(checkout)) {
            EXPECT_EQ(liquid::store_detail::lease_holder_state(record.owner),
                      liquid::store_detail::holder_state::alive)
                << record.owner;
        }
    }
    EXPECT_EQ(lease_files(checkout), 0U);
}

// Every open scans the pack it may append to, and every open checkpoints, so small packs keep a
// thousand opens from rescanning one growing pack each time.
liquid::repository_options small_packs() {
    auto options = liquid::repository_options{};
    options.storage.pack_size = 1024 * 1024;
    return options;
}

TEST(RepositoryLeases, AThousandOpensLeaveNoMoreLeasesThanLiveHolders) {
    const auto checkout = checkout_with_three_files("repository-leases-thousand");
    // A handle that stays open the whole time, like an IDE beside the daemon.
    auto keeper = liquid::runtime::block_on(liquid::repository::open(checkout, small_packs()));
    ASSERT_TRUE(keeper) << keeper.error().message;
    const auto held_by_keeper = lease_files(checkout);
    ASSERT_GT(held_by_keeper, 0U);
    auto most_seen = std::size_t{};
    for (auto round = 0; round != 1000; ++round) {
        auto opened = liquid::runtime::block_on(liquid::repository::open(checkout, small_packs()));
        ASSERT_TRUE(opened) << "round " << round << ": " << opened.error().message;
        // Every tenth handle also writes, alternating directories. Each write adds a change to its
        // directory's document, which every later open loads and saves when it checkpoints, so a
        // write in every round made the thousand opens quadratic (liquid#547). The leases are
        // taken by the open either way.
        if (round % 10 == 9) {
            auto written = liquid::runtime::block_on(
                opened->write(round / 10 % 2 == 0 ? "a.txt" : "notes/c.txt",
                              bytes_of("round " + std::to_string(round) + "\n")));
            ASSERT_TRUE(written) << "round " << round << ": " << written.error().message;
        }
        // Two live handles: the keeper and this one.
        most_seen = std::max(most_seen, lease_files(checkout));
    }
    EXPECT_LE(most_seen, 2 * held_by_keeper + 2);
    EXPECT_EQ(lease_files(checkout), held_by_keeper);
    auto counted = liquid::runtime::block_on(keeper->lease_count());
    ASSERT_TRUE(counted) << counted.error().message;
    EXPECT_EQ(*counted, held_by_keeper);
    *keeper = liquid::repository{};
    EXPECT_EQ(lease_files(checkout), 0U);

    // The last write to each directory is still there.
    auto reopened = liquid::runtime::block_on(liquid::repository::open(checkout));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto read = liquid::runtime::block_on(reopened->read("notes/c.txt"));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, bytes_of("round 999\n"));
    auto read_root = liquid::runtime::block_on(reopened->read("a.txt"));
    ASSERT_TRUE(read_root) << read_root.error().message;
    EXPECT_EQ(*read_root, bytes_of("round 989\n"));
}

TEST(RepositoryLeases, LeasesOfOtherHostsAndOtherRepositoriesAreKept) {
    const auto checkout = checkout_with_three_files("repository-leases-kept");
    const auto root = checkout / ".liquid-store";
    const auto backend = liquid::make_disk_store_backend(root);
    std::string scope;
    liquid::digest leased{};
    {
        auto opened = liquid::runtime::block_on(liquid::repository::open(checkout));
        ASSERT_TRUE(opened) << opened.error().message;
        const auto records = lease_records(checkout);
        ASSERT_FALSE(records.empty());
        scope = records.front().owner.substr(0, records.front().owner.find('\n') + 1);
        leased = records.front().root;
    }
    // A holder on a host whose processes this one cannot see, and a lease with no holder at all.
    for (const auto& owner :
         {scope + "checkpoint\nholder another-host.invalid 1 1", scope + "checkpoint"}) {
        auto created = liquid::runtime::block_on(liquid::store_detail::create_lease_until(
            backend, root / "leases", leased, std::nullopt, owner));
        ASSERT_TRUE(created) << created.error().message;
    }
    auto opened = liquid::runtime::block_on(liquid::repository::open(checkout));
    ASSERT_TRUE(opened) << opened.error().message;
    opened = liquid::repository{};
    EXPECT_EQ(lease_files(checkout), 2U);
}

#ifndef _WIN32

TEST(RepositoryLeases, AKilledProcessesLeasesAreCollectedOnTheNextOpen) {
    const auto checkout = checkout_with_three_files("repository-leases-killed");
    const auto output = checkout.parent_path() / "killed.out";
    const auto child = ::fork();
    if (child == 0) {
        std::freopen(output.c_str(), "w", stdout);
        ::execl(LIQUID_STORE_WRITERS_FIXTURE,
                LIQUID_STORE_WRITERS_FIXTURE,
                "killed",
                checkout.c_str(),
                static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status)) << "status " << status;
    const auto orphaned = lease_files(checkout);
    ASSERT_GT(orphaned, 0U) << "a killed handle cannot release its leases";
    for (const auto& record : lease_records(checkout)) {
        EXPECT_EQ(liquid::store_detail::lease_holder_state(record.owner),
                  liquid::store_detail::holder_state::dead)
            << record.owner;
    }

    auto opened = liquid::runtime::block_on(liquid::repository::open(checkout));
    ASSERT_TRUE(opened) << opened.error().message;
    for (const auto& record : lease_records(checkout)) {
        EXPECT_EQ(liquid::store_detail::lease_holder_state(record.owner),
                  liquid::store_detail::holder_state::alive)
            << "a dead holder's lease survived the open: " << record.owner;
    }
    // The killed handle's write was published before it died and is still there.
    auto read = liquid::runtime::block_on(opened->read("crashed.txt"));
    ASSERT_TRUE(read) << read.error().message;
    opened = liquid::repository{};
    EXPECT_EQ(lease_files(checkout), 0U);
}

#endif

} // namespace
