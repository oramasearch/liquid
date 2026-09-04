#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include "harness.hpp"

namespace {

std::uint64_t environment_number(const char *name, std::uint64_t fallback) {
  const auto *text = std::getenv(name);
  if (!text || *text == '\0')
    return fallback;
  char *end = nullptr;
  errno = 0;
  const auto value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0')
    throw std::runtime_error(std::string(name) + " must be an unsigned decimal integer");
  return value;
}

TEST(StoreSimulation, SeededCrashRecovery) {
  const auto *single_seed = std::getenv("LIQUID_SIM_SEED");
  const auto first = single_seed ? environment_number("LIQUID_SIM_SEED", 0) : 0;
  const auto count = single_seed ? 1 : environment_number("LIQUID_SIM_SEEDS", 200);
  const auto workspace = std::filesystem::current_path() / "liquid-sim-work";
  std::filesystem::create_directories(workspace);
  for (std::uint64_t offset = 0; offset < count; ++offset) {
    const auto seed = first + offset;
    const auto outcome = liquid::sim::run_store_simulation(seed, workspace);
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

} // namespace
