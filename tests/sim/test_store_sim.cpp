#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "../temporary_directory.hpp"
#include "faults.hpp"
#include "harness.hpp"
#include "memory_backend.hpp"

namespace {

std::uint64_t environment_number(const char* name, std::uint64_t fallback) {
    const auto* text = std::getenv(name);
    if (!text || *text == '\0')
        return fallback;
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        throw std::runtime_error(std::string(name) + " must be an unsigned decimal integer");
    return value;
}

class StoreSimulation : public testing::TestWithParam<liquid::sim::simulation_backend> {};

TEST_P(StoreSimulation, SeededCrashRecovery) {
    const auto* single_seed = std::getenv("LIQUID_SIM_SEED");
    const auto first = single_seed ? environment_number("LIQUID_SIM_SEED", 0) : 0;
    const auto count = single_seed ? 1 : environment_number("LIQUID_SIM_SEEDS", 200);
    const auto workspace = liquid::test::fixture_path("liquid-sim-work");
    std::filesystem::create_directories(workspace);
    for (std::uint64_t offset = 0; offset < count; ++offset) {
        const auto seed = first + offset;
        const auto outcome = liquid::sim::run_store_simulation(seed, workspace, GetParam());
        std::cout << "seed=" << seed << " fault_step=" << outcome.fault_step
                  << " orphans=" << outcome.orphan_count << '\n';
        ASSERT_FALSE(outcome.failure.has_value())
            << "seed=" << seed << " fault_step=" << outcome.fault_step
            << " orphans=" << outcome.orphan_count << "\n"
            << (outcome.failure ? outcome.failure->message : std::string{}) << "\n"
            << "replay: LIQUID_SIM_SEED=" << seed
            << " ctest --test-dir build -R StoreSimulation --output-on-failure";
    }
    std::filesystem::remove_all(workspace);
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         StoreSimulation,
                         testing::Values(liquid::sim::simulation_backend::disk,
                                         liquid::sim::simulation_backend::memory,
                                         liquid::sim::simulation_backend::write_once_disk,
                                         liquid::sim::simulation_backend::write_once_memory),
                         [](const auto& parameter) -> std::string {
                             switch (parameter.param) {
                             case liquid::sim::simulation_backend::disk:
                                 return "Disk";
                             case liquid::sim::simulation_backend::memory:
                                 return "Memory";
                             case liquid::sim::simulation_backend::write_once_disk:
                                 return "WriteOnceDisk";
                             case liquid::sim::simulation_backend::write_once_memory:
                                 return "WriteOnceMemory";
                             }
                             return "Unknown";
                         });

// liquid #344: the same seeded schedule, but the crash also loses writes since the last barrier.
// `Prefix` keeps a prefix of their issue order, which lake's workspace store gives today; `Subset`
// keeps any subset with each key's own writes in order, which is store_backend's contract
// (include/liquid/store.hpp). Every check of SeededCrashRecovery must hold under both.
using durability_case = std::tuple<liquid::sim::simulation_backend, liquid::sim::crash_durability>;

class StoreCrashDurability : public testing::TestWithParam<durability_case> {};

TEST_P(StoreCrashDurability, SeededCrashRecovery) {
    const auto* single_seed = std::getenv("LIQUID_SIM_SEED");
    const auto first = single_seed ? environment_number("LIQUID_SIM_SEED", 0) : 0;
    const auto count = single_seed ? 1 : environment_number("LIQUID_SIM_SEEDS", 200);
    const auto workspace = liquid::test::fixture_path("liquid-sim-durability-work");
    std::filesystem::create_directories(workspace);
    const auto [backend, durability] = GetParam();
    for (std::uint64_t offset = 0; offset < count; ++offset) {
        const auto seed = first + offset;
        const auto outcome =
            liquid::sim::run_store_simulation(seed, workspace, backend, durability);
        ASSERT_FALSE(outcome.failure.has_value())
            << "seed=" << seed << " fault_step=" << outcome.fault_step
            << " orphans=" << outcome.orphan_count << "\n"
            << (outcome.failure ? outcome.failure->message : std::string{}) << "\n"
            << "replay: LIQUID_SIM_SEED=" << seed
            << " ctest --test-dir build -R StoreCrashDurability --output-on-failure";
    }
    std::filesystem::remove_all(workspace);
}

INSTANTIATE_TEST_SUITE_P(
    Backends,
    StoreCrashDurability,
    testing::Combine(testing::Values(liquid::sim::simulation_backend::memory,
                                     liquid::sim::simulation_backend::write_once_memory),
                     testing::Values(liquid::sim::crash_durability::prefix,
                                     liquid::sim::crash_durability::subset)),
    [](const auto& parameter) -> std::string {
        const auto backend = std::get<0>(parameter.param) == liquid::sim::simulation_backend::memory
                                 ? std::string{"Memory"}
                                 : std::string{"WriteOnceMemory"};
        return backend + (std::get<1>(parameter.param) == liquid::sim::crash_durability::prefix
                              ? "Prefix"
                              : "Subset");
    });

// liquid #344: a compaction crashed at each of its fault points, on a backend that then keeps a
// prefix of the writes since the last barrier (`Prefix`), or any subset of them with each key's own
// writes in order (`Subset`, store_backend's contract), for several seeds. Recovery must
// leave exactly one of the old pack and its replacement: an uncommitted replacement that outlives
// its pending marker is an orphan pack the collector never accounts for.
class crash_at_fault final : public liquid::store_detail::fault_injector {
public:
    crash_at_fault(std::uint64_t target, std::function<void()> on_crash)
        : target_(target), on_crash_(std::move(on_crash)) {}

    void visit(liquid::store_detail::fault_event event) override {
        ++steps_;
        paths_.push_back(event.path.generic_string());
        if (steps_ != target_) {
            return;
        }
        where_ = event.path.generic_string();
        on_crash_();
        throw std::runtime_error("simulated compaction crash");
    }
    [[nodiscard]] std::uint64_t steps() const noexcept {
        return steps_;
    }
    [[nodiscard]] const std::string& where() const noexcept {
        return where_;
    }
    [[nodiscard]] const std::vector<std::string>& paths() const noexcept {
        return paths_;
    }

private:
    std::uint64_t target_{};
    std::function<void()> on_crash_;
    std::uint64_t steps_{};
    std::string where_;
    std::vector<std::string> paths_;
};

class fault_guard {
public:
    explicit fault_guard(liquid::store_detail::fault_injector* injector)
        : previous_(liquid::store_detail::set_fault_injector(injector)) {}
    ~fault_guard() {
        liquid::store_detail::set_fault_injector(previous_);
    }
    fault_guard(const fault_guard&) = delete;
    fault_guard& operator=(const fault_guard&) = delete;

private:
    liquid::store_detail::fault_injector* previous_{};
};

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    auto output = std::vector<std::byte>(size);
    auto value = seed * 2654435761U + 1U;
    for (auto& item : output) {
        value = value * 1103515245U + 12345U;
        item = std::byte{static_cast<unsigned char>(value >> 16U)};
    }
    return output;
}

std::set<std::string> pack_keys(liquid::sim::memory_store_backend& backend) {
    auto output = std::set<std::string>{};
    for (const auto& key : backend.keys()) {
        if (key.starts_with("packs/pack-")) {
            output.insert(key);
        }
    }
    return output;
}

struct compaction_fixture {
    std::shared_ptr<liquid::sim::memory_store_backend> backend =
        std::make_shared<liquid::sim::memory_store_backend>();
    std::optional<liquid::store> objects;
    std::optional<liquid::lease> root;
    liquid::digest kept{};

    [[nodiscard]] liquid::result<void> open(const std::filesystem::path& path) {
        objects.reset();
        auto options = liquid::store_options{};
        options.pack_size = 64U * 1024U;
        options.backend = backend;
        auto opened = liquid::runtime::block_on(liquid::store::open(path, options));
        if (!opened) {
            return std::unexpected(opened.error());
        }
        objects.emplace(std::move(*opened));
        return {};
    }

    // A pack with one rooted object and three unrooted ones, collected once without compaction so
    // the next compaction-only pass rewrites it.
    [[nodiscard]] liquid::result<void> prepare(const std::filesystem::path& path) {
        if (auto opened = open(path); !opened) {
            return opened;
        }
        auto id = liquid::runtime::block_on(objects->put(pattern(4096, 1)));
        if (!id) {
            return std::unexpected(id.error());
        }
        kept = *id;
        auto leased = liquid::runtime::block_on(objects->lease_root(kept, std::chrono::hours{1}));
        if (!leased) {
            return std::unexpected(leased.error());
        }
        root.emplace(std::move(*leased));
        for (auto index = 2U; index != 5U; ++index) {
            auto orphan = liquid::runtime::block_on(objects->put(pattern(4096, index)));
            if (!orphan) {
                return std::unexpected(orphan.error());
            }
        }
        auto options = liquid::collection_options{};
        options.compact_below = 0.0;
        auto collected = liquid::runtime::block_on(objects->collect(options));
        if (!collected) {
            return std::unexpected(collected.error());
        }
        return {};
    }

    [[nodiscard]] liquid::result<liquid::collection_report> compact() {
        auto options = liquid::collection_options{};
        options.compact_only = true;
        return liquid::runtime::block_on(objects->collect(options));
    }
};

class StoreCompactionCrashDurability
    : public testing::TestWithParam<liquid::sim::crash_durability> {};

TEST_P(StoreCompactionCrashDurability, NoOrphanPackAfterTheCrash) {
    const auto seeds = environment_number("LIQUID_COMPACTION_CRASH_SEEDS", 8);
    const auto durability = GetParam();
    const auto path = liquid::test::fixture_path("liquid-compaction-crash-durability");
    auto before = std::set<std::string>{};
    auto after = std::set<std::string>{};
    auto steps = std::uint64_t{};
    {
        auto probe = compaction_fixture{};
        auto prepared = probe.prepare(path);
        ASSERT_TRUE(prepared) << prepared.error().message;
        before = pack_keys(*probe.backend);
        auto counter = crash_at_fault{0, [] {}};
        {
            auto guard = fault_guard{&counter};
            auto compacted = probe.compact();
            ASSERT_TRUE(compacted) << compacted.error().message;
            ASSERT_GT(compacted->packs_compacted, 0U) << "the pass must rewrite a pack";
        }
        steps = counter.steps();
        after = pack_keys(*probe.backend);
    }
    ASSERT_NE(before, after);
    auto old_packs = std::set<std::string>{};
    std::ranges::set_difference(before, after, std::inserter(old_packs, old_packs.end()));
    auto new_packs = std::set<std::string>{};
    std::ranges::set_difference(after, before, std::inserter(new_packs, new_packs.end()));
    ASSERT_FALSE(old_packs.empty());
    ASSERT_FALSE(new_packs.empty());
    for (auto target = std::uint64_t{1}; target <= steps; ++target) {
        for (auto seed = std::uint64_t{}; seed != seeds; ++seed) {
            auto fixture = compaction_fixture{};
            auto prepared = fixture.prepare(path);
            ASSERT_TRUE(prepared) << prepared.error().message;
            auto crash = crash_at_fault{target, [&] { fixture.backend->set_frozen(true); }};
            {
                auto guard = fault_guard{&crash};
                static_cast<void>(fixture.compact());
            }
            fixture.root.reset();
            fixture.objects.reset();
            fixture.backend->set_frozen(false);
            const auto survived = fixture.backend->crash(durability, seed * 7919U + target);
            const auto where = "crash at compaction fault point " + std::to_string(target) +
                               " of " + std::to_string(steps) + " (" + crash.where() + "), seed " +
                               std::to_string(seed) + ", " + std::to_string(survived) +
                               " unbarriered writes kept";
            auto reopened = fixture.open(path);
            ASSERT_TRUE(reopened) << where << ": " << reopened.error().message;
            auto read = liquid::runtime::block_on(fixture.objects->get(fixture.kept));
            ASSERT_TRUE(read) << where << ": " << read.error().message;
            EXPECT_EQ(*read, pattern(4096, 1)) << where;
            const auto packs = pack_keys(*fixture.backend);
            const auto has = [&](const std::set<std::string>& keys) {
                return std::ranges::any_of(keys,
                                           [&](const auto& key) { return packs.contains(key); });
            };
            EXPECT_FALSE(has(old_packs) && has(new_packs))
                << where << ": the old pack and its replacement both survived recovery";
            EXPECT_TRUE(has(old_packs) || has(new_packs)) << where << ": no copy survived";
            fixture.objects.reset();
        }
    }
}

// liquid #344: recovery of an uncommitted compaction removes the replacement pack and then the
// pending marker. A crash during that recovery, on a backend that keeps any subset of the writes
// since the last barrier, must still leave the replacement to the next open: the marker may not go
// before the pack it names.
TEST(StoreCompactionRecoveryCrash, NoOrphanPackAfterASubsetCrashDuringRecovery) {
    const auto seeds = environment_number("LIQUID_COMPACTION_CRASH_SEEDS", 8);
    const auto path = liquid::test::fixture_path("liquid-compaction-recovery-crash");
    auto before = std::set<std::string>{};
    auto commit = std::uint64_t{};
    {
        auto probe = compaction_fixture{};
        auto prepared = probe.prepare(path);
        ASSERT_TRUE(prepared) << prepared.error().message;
        before = pack_keys(*probe.backend);
        auto counter = crash_at_fault{0, [] {}};
        {
            auto guard = fault_guard{&counter};
            auto compacted = probe.compact();
            ASSERT_TRUE(compacted) << compacted.error().message;
        }
        const auto& paths = counter.paths();
        const auto found = std::ranges::find(paths, std::string{"gc/MANIFEST"});
        ASSERT_NE(found, paths.end()) << "the compaction commits no collection manifest";
        commit = static_cast<std::uint64_t>(std::distance(paths.begin(), found)) + 1;
    }
    // Crashed just before the commit, with every write kept: the marker, the old pack and the
    // sealed replacement are all there, and the next open must remove the replacement.
    const auto interrupted = [&](compaction_fixture& fixture) {
        auto prepared = fixture.prepare(path);
        ASSERT_TRUE(prepared) << prepared.error().message;
        auto crash = crash_at_fault{commit, [&] { fixture.backend->set_frozen(true); }};
        {
            auto guard = fault_guard{&crash};
            static_cast<void>(fixture.compact());
        }
        fixture.root.reset();
        fixture.objects.reset();
        fixture.backend->set_frozen(false);
        static_cast<void>(fixture.backend->crash(liquid::sim::crash_durability::everything, 0));
    };
    auto steps = std::uint64_t{};
    {
        auto probe = compaction_fixture{};
        interrupted(probe);
        ASSERT_GT(pack_keys(*probe.backend).size(), before.size())
            << "the interrupted compaction left no replacement";
        auto counter = crash_at_fault{0, [] {}};
        {
            auto guard = fault_guard{&counter};
            auto reopened = probe.open(path);
            ASSERT_TRUE(reopened) << reopened.error().message;
        }
        steps = counter.steps();
        EXPECT_EQ(pack_keys(*probe.backend), before);
    }
    for (auto target = std::uint64_t{1}; target <= steps; ++target) {
        for (auto seed = std::uint64_t{}; seed != seeds; ++seed) {
            auto fixture = compaction_fixture{};
            interrupted(fixture);
            auto crash = crash_at_fault{target, [&] { fixture.backend->set_frozen(true); }};
            {
                auto guard = fault_guard{&crash};
                static_cast<void>(fixture.open(path));
            }
            fixture.objects.reset();
            fixture.backend->set_frozen(false);
            const auto survived = fixture.backend->crash(liquid::sim::crash_durability::subset,
                                                         seed * 7919U + target);
            const auto where = "crash at recovery fault point " + std::to_string(target) + " of " +
                               std::to_string(steps) + " (" + crash.where() + "), seed " +
                               std::to_string(seed) + ", " + std::to_string(survived) +
                               " unbarriered writes kept";
            auto reopened = fixture.open(path);
            ASSERT_TRUE(reopened) << where << ": " << reopened.error().message;
            auto read = liquid::runtime::block_on(fixture.objects->get(fixture.kept));
            ASSERT_TRUE(read) << where << ": " << read.error().message;
            EXPECT_EQ(pack_keys(*fixture.backend), before)
                << where << ": the uncommitted replacement outlived recovery";
            fixture.objects.reset();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Modes,
                         StoreCompactionCrashDurability,
                         testing::Values(liquid::sim::crash_durability::prefix,
                                         liquid::sim::crash_durability::subset),
                         [](const auto& parameter) -> std::string {
                             return parameter.param == liquid::sim::crash_durability::prefix
                                        ? "Prefix"
                                        : "Subset";
                         });

TEST(StoreBackend, MemoryBackendInjectsLatencyAndFailures) {
    auto backend = std::make_shared<liquid::sim::memory_store_backend>();
    backend->set_latency(std::chrono::milliseconds{1});
    backend->fail_next(
        {liquid::errc::unavailable, "injected backend failure", "memory store backend"});
    auto failed = liquid::runtime::block_on(backend->put("object", {}));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, liquid::errc::unavailable);
    ASSERT_TRUE(liquid::runtime::block_on(backend->put("object", {std::byte{7}})));
    auto read = liquid::runtime::block_on(backend->get_range("object", 0, 1));
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, std::vector<std::byte>{std::byte{7}});
}

TEST(StoreBackend, MemoryBackendPersistsStoreWithoutBackendFiles) {
    const auto path = liquid::test::fixture_path("liquid-memory-backend-test");
    std::filesystem::remove_all(path);
    auto backend = std::make_shared<liquid::sim::memory_store_backend>();
    liquid::store_options options;
    options.backend = backend;
    auto opened = liquid::runtime::block_on(liquid::store::open(path, options));
    ASSERT_TRUE(opened);
    const auto bytes = std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}};
    auto id = liquid::runtime::block_on(opened->put(bytes));
    ASSERT_TRUE(id);
    *opened = {};
    opened = liquid::runtime::block_on(liquid::store::open(path, options));
    ASSERT_TRUE(opened);
    auto read = liquid::runtime::block_on(opened->get(*id));
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, bytes);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(StoreBackend, DiskBackendImplementsContract) {
    const auto path = liquid::test::fixture_path("liquid-disk-backend-test");
    std::filesystem::remove_all(path);
    auto backend = liquid::make_disk_store_backend(path);
    ASSERT_TRUE(
        liquid::runtime::block_on(backend->put("packs/object", {std::byte{1}, std::byte{2}})));
    ASSERT_TRUE(liquid::runtime::block_on(backend->put_range("packs/object", 2, {std::byte{3}})));
    auto read = liquid::runtime::block_on(backend->get_range("packs/object", 1, 2));
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, (std::vector<std::byte>{std::byte{2}, std::byte{3}}));
    auto entries = liquid::runtime::block_on(backend->list("packs/"));
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().key, "packs/object");
    EXPECT_EQ(entries->front().size, 3U);
    EXPECT_TRUE(liquid::runtime::block_on(backend->barrier()));
    EXPECT_TRUE(liquid::runtime::block_on(backend->remove("packs/object")));
    auto missing = liquid::runtime::block_on(backend->get_range("packs/object", 0, 1));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);
    std::filesystem::remove_all(path);
}

} // namespace
