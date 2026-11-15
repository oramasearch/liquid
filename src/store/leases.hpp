// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {
class store_backend;
}

namespace liquid::store_detail {

struct lease_record {
    std::string id;
    digest root{};
    std::chrono::sys_seconds expiry{};
    std::string owner;
    std::uint64_t generation{};
    std::string key;
};

struct lease_state {
    std::shared_ptr<::liquid::store_backend> backend;
    lease_record record;
    bool released{};
};

[[nodiscard]] runtime::task<std::shared_ptr<lease_state>>
create_lease_until(std::shared_ptr<::liquid::store_backend> backend,
                   std::filesystem::path context,
                   digest root,
                   std::optional<std::chrono::sys_seconds> expiry,
                   std::string owner);
[[nodiscard]] runtime::task<void> renew_lease(std::shared_ptr<lease_state> state,
                                              std::chrono::seconds ttl);
[[nodiscard]] runtime::task<void> release_lease(std::shared_ptr<lease_state> state);
[[nodiscard]] runtime::task<std::vector<lease_record>>
load_leases(std::shared_ptr<::liquid::store_backend> backend, std::filesystem::path context);

} // namespace liquid::store_detail
