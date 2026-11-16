// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT node_store_options {
    std::filesystem::path root;
    std::string listen{"unix:///run/liquid/node-store.sock"};
    std::string control_listen{"127.0.0.1:8082"};
    std::uint64_t budget_bytes{};
};

struct LIQUID_EXPORT mount_request {
    digest tree;
    std::string store_url;
    std::string bearer;
};

struct LIQUID_EXPORT mount_result {
    std::filesystem::path path;
    digest tree;
    std::uint64_t records_fetched{};
    std::uint64_t bytes_remote{};
};

struct LIQUID_EXPORT unmount_request {
    std::optional<digest> tree;
    std::optional<std::filesystem::path> path;
};

struct LIQUID_EXPORT prefetch_request {
    digest tree;
};

struct LIQUID_EXPORT node_store_mount {
    digest tree;
    std::filesystem::path path;
    std::uint32_t references{};
    std::uint64_t chunks_fetched{};
    std::uint64_t bytes_fetched{};
    std::uint64_t bytes_avoided{};
    std::uint64_t hydrated_bytes{};
};

struct LIQUID_EXPORT node_store_status {
    std::uint64_t store_bytes{};
    std::uint64_t budget_bytes{};
    std::uint64_t evictions{};
    std::vector<node_store_mount> mounts;
    std::optional<std::string> last_error;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_node_store(node_store_options options);

} // namespace liquid
