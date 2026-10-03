#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace liquid::sim {

// The write-once kinds decorate the disk and memory backends with write_once_store_backend, which
// refuses any range that does not append, as Zetta's object storage does (liquid #332).
enum class simulation_backend { disk, memory, write_once_disk, write_once_memory };

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
