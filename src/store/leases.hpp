#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid::store_detail {

struct lease_record {
    std::string id;
    digest root{};
    std::chrono::sys_seconds expiry{};
    std::string owner;
    std::uint64_t generation{};
    std::filesystem::path path;
};

struct lease_state {
    std::filesystem::path directory;
    lease_record record;
    bool released{};
};

[[nodiscard]] runtime::task<std::shared_ptr<lease_state>>
create_lease(std::filesystem::path directory, digest root, std::chrono::seconds ttl);
[[nodiscard]] runtime::task<void> renew_lease(std::shared_ptr<lease_state> state,
                                              std::chrono::seconds ttl);
[[nodiscard]] runtime::task<void> release_lease(std::shared_ptr<lease_state> state);
[[nodiscard]] runtime::task<std::vector<lease_record>> load_leases(std::filesystem::path directory);

} // namespace liquid::store_detail
