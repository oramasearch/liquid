// liquid #321: one open handle owns a store root at a time, across processes, and a successor
// can wait (bounded) for a draining or killed predecessor to release it.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "owner_fixture_content.hpp"
#include "store/backend.hpp"
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
