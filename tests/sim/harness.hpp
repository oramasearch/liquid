#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace liquid::sim {

enum class simulation_backend { disk, memory };

struct simulation_failure {
    std::uint64_t seed{};
    std::uint64_t fault_step{};
    std::string message;
};

struct simulation_result {
    std::uint64_t fault_step{};
    std::uint64_t orphan_count{};
    std::optional<simulation_failure> failure;
};

[[nodiscard]] simulation_result
run_store_simulation(std::uint64_t seed,
                     const std::filesystem::path& workspace,
                     simulation_backend backend = simulation_backend::disk);

} // namespace liquid::sim
