#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "conformance_operations.hpp"
#include "vfs/backend.hpp"

namespace liquid::test::vfs_conformance {

struct mount_request {
    repository& source;
    std::filesystem::path mountpoint;
    std::optional<digest> input_tree;
    std::vector<std::string> outputs;
    vfs_detail::observation_sink observe;
    bool read_only{};
};

struct backend_instance {
    std::shared_ptr<vfs_detail::backend> lifecycle;
#if !defined(_WIN32)
    std::shared_ptr<filesystem_operations> operations;
#endif
};

// Each backend supplies lifecycle, operations, counters and observation delivery.
// All expectations stay in the common suite.
struct backend_driver {
    std::string_view name;
    runtime::task<backend_instance> (*start)(mount_request);
};

auto start_direct(mount_request request) -> runtime::task<backend_instance>;

auto backends() -> std::span<const backend_driver>;
extern const backend_driver* selected_backend;
extern bool allow_unavailable;

} // namespace liquid::test::vfs_conformance
