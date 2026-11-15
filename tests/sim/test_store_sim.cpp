// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../temporary_directory.hpp"
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
