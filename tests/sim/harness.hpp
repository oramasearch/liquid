#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "memory_backend.hpp"

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

// `durability` is what the crash keeps of the writes since the last barrier (liquid #344). Anything
// but `everything` needs a memory backend kind: the backend freezes at the crash point and then
// keeps the part of its unbarriered log that the mode and the seed choose.
[[nodiscard]] simulation_result
run_store_simulation(std::uint64_t seed,
                     const std::filesystem::path& workspace,
                     simulation_backend backend = simulation_backend::disk,
                     crash_durability durability = crash_durability::everything);

} // namespace liquid::sim
