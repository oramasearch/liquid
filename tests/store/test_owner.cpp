// liquid #321: one open handle owns a store root at a time, across processes, and a successor
// can wait (bounded) for a draining or killed predecessor to release it. liquid #327: the same
// ownership through an injected owner whose grant carries a fence, on a remote-shaped backend.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "owner_fixture_content.hpp"
#include "remote_backend.hpp"
#include "store/backend.hpp"
#include "store/owner.hpp"
#include "store/pack.hpp"

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

liquid::store_options exclusive(std::chrono::milliseconds wait = std::chrono::milliseconds{0}) {
    liquid::store_options options;
    options.exclusive_owner = true;
    options.owner_wait = wait;
    return options;
}

// A persistence backend with no local root. Exclusive ownership is refused before any call.
class rootless_backend final : public liquid::store_backend {
public:
    liquid::runtime::task<void> put(std::string, std::vector<std::byte>) override {
        co_return refused();
    }
    liquid::runtime::task<void>
    put_range(std::string, std::uint64_t, std::vector<std::byte>) override {
        co_return refused();
    }
    liquid::runtime::task<std::vector<std::byte>>
    get_range(std::string, std::uint64_t, std::uint64_t) override {
        co_return refused();
    }
    liquid::runtime::task<void> remove(std::string) override {
        co_return refused();
    }
    liquid::runtime::task<std::vector<liquid::store_backend_entry>> list(std::string) override {
        co_return refused();
    }
    liquid::runtime::task<void> barrier() override {
        co_return refused();
    }

private:
    static std::unexpected<liquid::error> refused() {
        return std::unexpected(liquid::error{liquid::errc::internal, "not expected", "test"});
    }
};

std::filesystem::path fresh(const std::string& name) {
    const auto path = liquid::test::fixture_path(name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

TEST(StoreOwner, SecondOpenIsRefusedUntilTheOwnerIsDestroyed) {
    const auto root = fresh("store-owner-refused") / "store";
    auto first = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(first) << first.error().message;

    auto second = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, liquid::errc::locked);
    EXPECT_NE(second.error().message.find("pid "), std::string::npos) << second.error().message;
    EXPECT_EQ(second.error().context, (root / "store.lock").string());

    *first = {};
    auto third = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(third) << third.error().message;
}

TEST(StoreOwner, BoundedWaitExpiresWhileTheOwnerHoldsTheRoot) {
    const auto root = fresh("store-owner-expires") / "store";
    auto first = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(first) << first.error().message;

    const auto options = exclusive(std::chrono::milliseconds{150});
    const auto started = std::chrono::steady_clock::now();
    auto second = liquid::runtime::block_on(liquid::store::open(root, options));
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, liquid::errc::locked);
    EXPECT_GE(waited, std::chrono::milliseconds{150});
}

TEST(StoreOwner, WaitingOpenerTakesTheRootWhenTheOwnerReleasesIt) {
    const auto root = fresh("store-owner-handoff") / "store";
    auto first = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(first) << first.error().message;
    const std::vector<std::byte> payload(8192, std::byte{7});
    auto stored = liquid::runtime::block_on(first->put(payload));
    ASSERT_TRUE(stored) << stored.error().message;

    std::thread drain([&first] {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        *first = {};
    });
    const auto options = exclusive(std::chrono::seconds{30});
    auto successor = liquid::runtime::block_on(liquid::store::open(root, options));
    drain.join();
    ASSERT_TRUE(successor) << successor.error().message;
    auto read = liquid::runtime::block_on(successor->get(*stored));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, payload);
}

TEST(StoreOwner, InvalidOwnershipOptionsAreRejected) {
    const auto root = fresh("store-owner-invalid") / "store";
    auto negative = liquid::runtime::block_on(
        liquid::store::open(root, exclusive(std::chrono::milliseconds{-1})));
    ASSERT_FALSE(negative);
    EXPECT_EQ(negative.error().code, liquid::errc::invalid_argument);

    liquid::store_options wait_only;
    wait_only.owner_wait = std::chrono::seconds{1};
    auto unowned = liquid::runtime::block_on(liquid::store::open(root, wait_only));
    ASSERT_FALSE(unowned);
    EXPECT_EQ(unowned.error().code, liquid::errc::invalid_argument);

    auto memory = exclusive();
    memory.backend = std::make_shared<rootless_backend>();
    auto in_memory = liquid::runtime::block_on(liquid::store::open(root, memory));
    ASSERT_FALSE(in_memory);
    EXPECT_EQ(in_memory.error().code, liquid::errc::unsupported);
}

using liquid::test::remote_backend;
using liquid::test::remote_options;
using liquid::test::remote_owner;
using liquid::test::remote_service;
using liquid::test::remote_store;

// Several chunks, so one put makes several backend mutations.
std::vector<std::byte> large_payload() {
    std::vector<std::byte> output;
    for (std::uint64_t index = 0; output.size() < 2U * 1024U * 1024U; ++index) {
        const auto part = liquid::test::owner_fixture_bytes(327, index);
        output.insert(output.end(), part.begin(), part.end());
    }
    return output;
}

// The successor's view of a store that a stale owner wrote: it opens, a collection dry run passes,
// every acknowledged object reads back, and a collection over leases on them keeps every one.
void check_remote_store(
    const std::shared_ptr<remote_service>& service,
    const std::filesystem::path& root,
    const std::vector<std::pair<liquid::digest, std::vector<std::byte>>>& acked) {
    auto opened = remote_options(service);
    auto store = liquid::runtime::block_on(liquid::store::open(root, opened.options));
    ASSERT_TRUE(store) << store.error().message;
    auto dry = liquid::runtime::block_on(store->collect({.dry_run = true}));
    ASSERT_TRUE(dry) << dry.error().message;
    std::vector<liquid::lease> leases;
    for (const auto& [id, bytes] : acked) {
        auto read = liquid::runtime::block_on(store->get(id));
        ASSERT_TRUE(read) << read.error().message;
        ASSERT_EQ(*read, bytes);
        auto leased = liquid::runtime::block_on(store->lease_root(id, std::chrono::hours{1}));
        ASSERT_TRUE(leased) << leased.error().message;
        leases.push_back(std::move(*leased));
    }
    auto collected = liquid::runtime::block_on(store->collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_EQ(collected->live_objects, acked.size());
    auto closed = liquid::runtime::block_on(store->close());
    ASSERT_TRUE(closed) << closed.error().message;
}

TEST(StoreOwner, InjectedOwnerSuccessorWaitsAndTakesOverWhenThePredecessorCloses) {
    const auto root = fresh("store-owner-injected-handoff") / "store";
    auto service = std::make_shared<remote_service>();
    auto first_options = remote_options(service);
    auto first = liquid::runtime::block_on(liquid::store::open(root, first_options.options));
    ASSERT_TRUE(first) << first.error().message;
    const auto payload = liquid::test::owner_fixture_bytes(1, 1);
    auto stored = liquid::runtime::block_on(first->put(payload));
    ASSERT_TRUE(stored) << stored.error().message;

    // An opener that will not wait is refused and told who holds the store.
    auto refused_options = remote_options(service);
    auto refused = liquid::runtime::block_on(liquid::store::open(root, refused_options.options));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    EXPECT_NE(refused.error().message.find("pid "), std::string::npos) << refused.error().message;

    // The predecessor drains and closes while the successor waits.
    std::thread drain([&first] {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        auto closed = liquid::runtime::block_on(first->close());
        EXPECT_TRUE(closed) << closed.error().message;
    });
    auto successor_options = remote_options(service, std::chrono::seconds{30});
    auto successor =
        liquid::runtime::block_on(liquid::store::open(root, successor_options.options));
    drain.join();
    ASSERT_TRUE(successor) << successor.error().message;
    EXPECT_EQ(first_options.owner->releases, 1U);
    EXPECT_GT(successor_options.owner->grant().fence(), first_options.owner->grant().fence());

    auto read = liquid::runtime::block_on(successor->get(*stored));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, payload);
    const auto next = liquid::test::owner_fixture_bytes(1, 2);
    auto written = liquid::runtime::block_on(successor->put(next));
    ASSERT_TRUE(written) << written.error().message;
    auto closed = liquid::runtime::block_on(successor->close());
    ASSERT_TRUE(closed) << closed.error().message;
    EXPECT_EQ(service->refused, 0U);
    check_remote_store(service, root, {{*stored, payload}, {*written, next}});
}

TEST(StoreOwner, InjectedOwnerBoundedWaitExpires) {
    const auto root = fresh("store-owner-injected-expires") / "store";
    auto service = std::make_shared<remote_service>();
    auto first_options = remote_options(service);
    auto first = liquid::runtime::block_on(liquid::store::open(root, first_options.options));
    ASSERT_TRUE(first) << first.error().message;

    auto second_options = remote_options(service, std::chrono::milliseconds{150});
    const auto started = std::chrono::steady_clock::now();
    auto second = liquid::runtime::block_on(liquid::store::open(root, second_options.options));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, liquid::errc::locked);
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{150});
}

TEST(StoreOwner, StaleOwnerWhoseLeaseExpiredMidWriteIsRefusedAndTheSuccessorRecovers) {
    const auto root = fresh("store-owner-stale") / "store";
    auto service = std::make_shared<remote_service>();
    auto stale_options = remote_options(service);
    auto stale = liquid::runtime::block_on(liquid::store::open(root, stale_options.options));
    ASSERT_TRUE(stale) << stale.error().message;

    std::vector<std::pair<liquid::digest, std::vector<std::byte>>> acked;
    for (std::uint64_t index = 0; index != 3; ++index) {
        auto bytes = liquid::test::owner_fixture_bytes(2, index);
        auto stored = liquid::runtime::block_on(stale->put(bytes));
        ASSERT_TRUE(stored) << stored.error().message;
        acked.emplace_back(*stored, std::move(bytes));
    }

    // The lease lapses after the next accepted mutation, in the middle of a multi-chunk put; a
    // successor then takes the lease with a higher fence.
    {
        auto lock = std::scoped_lock(service->mutex);
        service->expire_at = service->accepted + 1;
    }
    const auto large = large_payload();
    auto interrupted = liquid::runtime::block_on(stale->put(large));
    ASSERT_FALSE(interrupted);
    EXPECT_EQ(interrupted.error().code, liquid::errc::locked) << interrupted.error().message;
    EXPECT_EQ(service->refused, 1U);
    EXPECT_TRUE(stale_options.owner->grant().lost());

    auto successor_options = remote_options(service);
    auto successor =
        liquid::runtime::block_on(liquid::store::open(root, successor_options.options));
    ASSERT_TRUE(successor) << successor.error().message;
    EXPECT_GT(successor_options.owner->grant().fence(), stale_options.owner->grant().fence());

    // The stale holder's later work is refused without reaching the remote, reads included.
    const auto accepted = service->accepted;
    auto again = liquid::runtime::block_on(stale->put(liquid::test::owner_fixture_bytes(2, 9)));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, liquid::errc::locked);
    auto stale_read = liquid::runtime::block_on(stale->get(acked.front().first));
    ASSERT_FALSE(stale_read);
    EXPECT_EQ(stale_read.error().code, liquid::errc::locked);
    EXPECT_EQ(service->refused, 1U);
    EXPECT_EQ(service->accepted, accepted);

    // The successor sees every acknowledged write and none of the interrupted one, and writes.
    const auto large_id = liquid::hash(liquid::algorithm::blake3, large);
    ASSERT_TRUE(large_id) << large_id.error().message;
    auto contains = liquid::runtime::block_on(successor->contains(*large_id));
    ASSERT_TRUE(contains) << contains.error().message;
    EXPECT_FALSE(*contains);
    auto bytes = liquid::test::owner_fixture_bytes(3, 0);
    auto written = liquid::runtime::block_on(successor->put(bytes));
    ASSERT_TRUE(written) << written.error().message;
    acked.emplace_back(*written, std::move(bytes));
    auto closed = liquid::runtime::block_on(successor->close());
    ASSERT_TRUE(closed) << closed.error().message;
    *stale = {};
    check_remote_store(service, root, acked);
}

TEST(StoreOwner, RevokedGrantRefusesTheNextMutationAndTheSuccessorTakesOver) {
    const auto root = fresh("store-owner-revoked") / "store";
    auto service = std::make_shared<remote_service>();
    auto revoked_options = remote_options(service);
    revoked_options.options.read_after_owner_lost = true;
    auto revoked = liquid::runtime::block_on(liquid::store::open(root, revoked_options.options));
    ASSERT_TRUE(revoked) << revoked.error().message;
    const auto payload = liquid::test::owner_fixture_bytes(4, 0);
    auto stored = liquid::runtime::block_on(revoked->put(payload));
    ASSERT_TRUE(stored) << stored.error().message;

    // The owner learns its lease is gone (a failed renewal) and revokes the grant.
    revoked_options.owner->grant().revoke();
    const auto accepted = service->accepted;
    auto refused = liquid::runtime::block_on(revoked->put(liquid::test::owner_fixture_bytes(4, 1)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    EXPECT_EQ(service->accepted, accepted);
    EXPECT_EQ(service->refused, 0U);
    // The caller opted in to reads after the loss.
    auto read = liquid::runtime::block_on(revoked->get(*stored));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, payload);

    // Destroying the store without close() still releases the owner, so a waiting successor gets
    // the store.
    std::thread drain([&revoked] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        *revoked = {};
    });
    auto successor_options = remote_options(service, std::chrono::seconds{30});
    auto successor =
        liquid::runtime::block_on(liquid::store::open(root, successor_options.options));
    drain.join();
    ASSERT_TRUE(successor) << successor.error().message;
    auto closed = liquid::runtime::block_on(successor->close());
    ASSERT_TRUE(closed) << closed.error().message;
    check_remote_store(service, root, {{*stored, payload}});
}

TEST(StoreOwner, StaleOwnersConditionalCreateIsRefused) {
    auto service = std::make_shared<remote_service>();
    auto owner = std::make_shared<remote_owner>(service);
    auto grant = liquid::runtime::block_on(owner->acquire("test", std::chrono::milliseconds{0}));
    ASSERT_TRUE(grant) << grant.error().message;
    auto view = remote_backend(service).fenced(grant->fence());
    ASSERT_TRUE(view) << view.error().message;
    auto owned = liquid::store_detail::owned_store_backend(*view, *grant, false);
    const auto bytes = liquid::test::owner_fixture_bytes(6, 0);

    auto created = liquid::runtime::block_on(owned.put_if_absent("a", bytes));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(service->written_with.at("a"), grant->fence());
    auto exists = liquid::runtime::block_on(owned.put_if_absent("a", bytes));
    ASSERT_FALSE(exists);
    EXPECT_EQ(exists.error().code, liquid::errc::already_exists);
    EXPECT_FALSE(grant->lost());

    // The lease lapses: the remote refuses the stale fence and the grant is revoked.
    {
        auto lock = std::scoped_lock(service->mutex);
        service->holder.reset();
    }
    auto stale = liquid::runtime::block_on(owned.put_if_absent("b", bytes));
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, liquid::errc::locked);
    EXPECT_EQ(service->refused, 1U);
    EXPECT_TRUE(grant->lost());

    // Later conditional creates are refused locally.
    auto again = liquid::runtime::block_on(owned.put_if_absent("c", bytes));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, liquid::errc::locked);
    EXPECT_EQ(service->refused, 1U);
    EXPECT_FALSE(service->objects.contains("b"));
    EXPECT_FALSE(service->objects.contains("c"));
}

TEST(StoreOwner, RepositoryStateWritesGoThroughTheFencedView) {
    const auto workspace = fresh("store-owner-repository");
    auto service = std::make_shared<remote_service>();
    auto opened = remote_options(service);
    auto store =
        liquid::runtime::block_on(liquid::store::open(workspace / "store", opened.options));
    ASSERT_TRUE(store) << store.error().message;
    const auto fence = opened.owner->grant().fence();

    auto repository = liquid::runtime::block_on(
        liquid::repository::create(workspace / "repository", {.store = &*store}));
    ASSERT_TRUE(repository) << repository.error().message;
    auto written = liquid::runtime::block_on(
        repository->write("notes.txt", liquid::test::owner_fixture_bytes(7, 0)));
    ASSERT_TRUE(written) << written.error().message;

    // Every repository key was written by the owner, with its fence, and the create was a
    // conditional create.
    std::size_t repository_keys = 0;
    bool marker = false;
    for (const auto& [key, carried] : service->written_with) {
        if (!key.starts_with("repositories/"))
            continue;
        ++repository_keys;
        marker = marker || key.ends_with("/LIQUID_REPO");
        EXPECT_EQ(carried, fence) << key;
    }
    EXPECT_GT(repository_keys, 0U);
    EXPECT_TRUE(marker);
    EXPECT_GE(service->created, 1U);
    EXPECT_EQ(service->refused, 0U);

    // Once the lease lapses, the repository's next write is refused.
    {
        auto lock = std::scoped_lock(service->mutex);
        service->holder.reset();
    }
    auto refused = liquid::runtime::block_on(
        repository->write("notes.txt", liquid::test::owner_fixture_bytes(7, 1)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked) << refused.error().message;
    EXPECT_TRUE(opened.owner->grant().lost());
}

TEST(StoreOwner, InjectedOwnerNeedsABackendThatCarriesTheFence) {
    const auto root = fresh("store-owner-unfenced") / "store";
    auto service = std::make_shared<remote_service>();
    auto options = remote_options(service);
    options.options.backend = std::make_shared<rootless_backend>();
    auto opened = liquid::runtime::block_on(liquid::store::open(root, options.options));
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, liquid::errc::unsupported);
    EXPECT_EQ(options.owner->releases, 1U);
    EXPECT_FALSE(service->holder);
}

TEST(StoreOwner, DiskOwnerCanBeInjectedAndCloseReleasesIt) {
    const auto root = fresh("store-owner-disk-injected") / "store";
    liquid::store_options injected;
    injected.owner = liquid::make_disk_store_owner(root);
    auto first = liquid::runtime::block_on(liquid::store::open(root, injected));
    ASSERT_TRUE(first) << first.error().message;
    auto stored = liquid::runtime::block_on(first->put(liquid::test::owner_fixture_bytes(5, 0)));
    ASSERT_TRUE(stored) << stored.error().message;

    auto second = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, liquid::errc::locked);

    auto closed = liquid::runtime::block_on(first->close());
    ASSERT_TRUE(closed) << closed.error().message;
    auto third = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(third) << third.error().message;
    auto read = liquid::runtime::block_on(third->get(*stored));
    ASSERT_TRUE(read) << read.error().message;
}

TEST(StoreOwner, OpensWithoutExclusiveOwnershipTakeNoLock) {
    const auto root = fresh("store-owner-unowned") / "store";
    auto unowned = liquid::runtime::block_on(liquid::store::open(root));
    ASSERT_TRUE(unowned) << unowned.error().message;
    EXPECT_FALSE(std::filesystem::exists(root / "store.lock"));
}

#ifndef _WIN32

struct writer {
    pid_t pid{-1};
    std::filesystem::path ack;
    std::uint64_t seed{};
};

writer start_writer(const std::filesystem::path& root,
                    const std::filesystem::path& ack,
                    std::uint64_t seed,
                    std::uint64_t count,
                    std::chrono::milliseconds wait) {
    const auto seed_text = std::to_string(seed);
    const auto count_text = std::to_string(count);
    const auto wait_text = std::to_string(wait.count());
    const auto child = ::fork();
    if (child == 0) {
        ::execl(LIQUID_STORE_OWNER_FIXTURE,
                LIQUID_STORE_OWNER_FIXTURE,
                root.c_str(),
                ack.c_str(),
                seed_text.c_str(),
                count_text.c_str(),
                wait_text.c_str(),
                static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    return writer{child, ack, seed};
}

int wait_for(const writer& value) {
    int status = 0;
    if (::waitpid(value.pid, &status, 0) != value.pid)
        return -1;
    return status;
}

bool running(const writer& value) {
    int status = 0;
    return ::waitpid(value.pid, &status, WNOHANG) == 0;
}

// Every complete "<index> <digest>" line of an acknowledgement file. A line torn by SIGKILL is
// not an acknowledgement and is ignored.
std::map<std::uint64_t, liquid::digest> acknowledged(const std::filesystem::path& ack) {
    std::map<std::uint64_t, liquid::digest> output;
    std::ifstream input(ack);
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::size_t start = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
        const auto line = text.substr(start, end - start);
        start = end + 1;
        const auto space = line.find(' ');
        if (space == std::string::npos)
            continue;
        auto parsed = liquid::parse_digest(line.substr(space + 1));
        if (parsed)
            output.emplace(std::stoull(line.substr(0, space)), *parsed);
    }
    return output;
}

std::size_t wait_for_acknowledgements(const writer& value, std::size_t minimum) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    std::size_t seen = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        seen = acknowledged(value.ack).size();
        if (seen >= minimum || !running(value))
            return seen;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return seen;
}

// fsck: the store reopens, every frame of every pack decodes and hashes to the digest its pack
// table records, and every acknowledged object reads back byte for byte.
void check_store(const std::filesystem::path& root, const std::vector<writer>& writers) {
    auto backend = liquid::make_disk_store_backend(root);
    auto packs = liquid::runtime::block_on(backend->list("packs/pack-"));
    ASSERT_TRUE(packs) << packs.error().message;
    std::size_t frames = 0;
    for (const auto& file : *packs) {
        const auto name = std::filesystem::path(file.key).filename().string();
        if (!name.ends_with(".pack"))
            continue;
        const auto id = static_cast<std::uint32_t>(std::stoul(name.substr(5, name.size() - 10)));
        auto pack =
            liquid::runtime::block_on(liquid::store_detail::read_pack(backend, file.key, id));
        ASSERT_TRUE(pack) << file.key << ": " << pack.error().message;
        for (const auto& entry : pack->entries) {
            ASSERT_FALSE(entry.base) << "the owner fixture writes no deltas";
            auto bytes = liquid::runtime::block_on(
                liquid::store_detail::read_pack_entry(backend, file.key, entry.location, entry.id));
            ASSERT_TRUE(bytes) << file.key << " frame at " << entry.location.offset << ": "
                               << bytes.error().message;
            ++frames;
        }
    }
    EXPECT_GT(frames, 0U);

    auto store = liquid::runtime::block_on(liquid::store::open(root, exclusive()));
    ASSERT_TRUE(store) << store.error().message;
    std::vector<liquid::lease> leases;
    std::size_t checked = 0;
    for (const auto& value : writers) {
        for (const auto& [index, id] : acknowledged(value.ack)) {
            auto read = liquid::runtime::block_on(store->get(id));
            ASSERT_TRUE(read) << "seed " << value.seed << " index " << index << ": "
                              << read.error().message;
            ASSERT_EQ(*read, liquid::test::owner_fixture_bytes(value.seed, index))
                << "seed " << value.seed << " index " << index;
            auto leased = liquid::runtime::block_on(store->lease_root(id, std::chrono::hours{1}));
            ASSERT_TRUE(leased) << leased.error().message;
            leases.push_back(std::move(*leased));
            ++checked;
        }
    }
    EXPECT_GT(checked, 0U);
    // A full collection walks every leased root and rewrites the index; every acknowledged
    // object must survive it too.
    auto collected = liquid::runtime::block_on(store->collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_EQ(collected->live_objects, checked);
    for (const auto& value : writers) {
        for (const auto& [index, id] : acknowledged(value.ack)) {
            auto read = liquid::runtime::block_on(store->get(id));
            ASSERT_TRUE(read) << read.error().message;
            ASSERT_EQ(*read, liquid::test::owner_fixture_bytes(value.seed, index));
        }
    }
}

TEST(StoreOwner, ConcurrentWriterProcessesTakeTurnsAndEveryWriteSurvives) {
    const auto workspace = fresh("store-owner-concurrent");
    const auto root = workspace / "store";
    std::vector<writer> writers;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        writers.push_back(start_writer(root,
                                       workspace / ("ack-" + std::to_string(seed)),
                                       seed,
                                       25,
                                       std::chrono::seconds{120}));
    }
    for (const auto& value : writers) {
        const auto status = wait_for(value);
        ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
        EXPECT_EQ(WEXITSTATUS(status), 0);
        EXPECT_EQ(acknowledged(value.ack).size(), 25U);
    }
    check_store(root, writers);
}

TEST(StoreOwner, SuccessorWaitsForAWriterKilledMidWriteAndEveryAcknowledgedWriteSurvives) {
    const auto workspace = fresh("store-owner-killed");
    const auto root = workspace / "store";

    // The predecessor writes until it is killed.
    const auto predecessor = start_writer(root, workspace / "ack-1", 1, 0, std::chrono::seconds{0});
    ASSERT_GE(wait_for_acknowledgements(predecessor, 40), 40U);
    ASSERT_TRUE(running(predecessor));

    // An opener that will not wait is refused and names the owner.
    const auto refused =
        start_writer(root, workspace / "ack-refused", 9, 1, std::chrono::seconds{0});
    const auto refused_status = wait_for(refused);
    ASSERT_TRUE(WIFEXITED(refused_status));
    EXPECT_EQ(WEXITSTATUS(refused_status), 3);
    EXPECT_TRUE(acknowledged(refused.ack).empty());

    // The successor starts while the predecessor is still writing and waits for the root.
    const auto successor =
        start_writer(root, workspace / "ack-2", 2, 30, std::chrono::seconds{120});
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    ASSERT_TRUE(running(successor));
    EXPECT_TRUE(acknowledged(successor.ack).empty())
        << "the successor wrote while the predecessor owned the root";

    // Kill the predecessor in the middle of its write stream.
    const auto before = acknowledged(predecessor.ack).size();
    ASSERT_GE(wait_for_acknowledgements(predecessor, before + 5), before + 5);
    ASSERT_EQ(::kill(predecessor.pid, SIGKILL), 0);
    const auto killed_status = wait_for(predecessor);
    ASSERT_TRUE(WIFSIGNALED(killed_status));
    EXPECT_EQ(WTERMSIG(killed_status), SIGKILL);

    const auto successor_status = wait_for(successor);
    ASSERT_TRUE(WIFEXITED(successor_status)) << "status " << successor_status;
    EXPECT_EQ(WEXITSTATUS(successor_status), 0);
    EXPECT_EQ(acknowledged(successor.ack).size(), 30U);

    check_store(root, {predecessor, successor});
}

#endif

} // namespace
