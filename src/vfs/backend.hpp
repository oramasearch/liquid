// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "liquid/vfs.hpp"
#include "vfs/observation.hpp"

namespace liquid::vfs_detail {

class backend {
public:
    virtual ~backend() = default;
    virtual void request_unmount() noexcept = 0;
    [[nodiscard]] virtual auto unmount() -> runtime::task<void> = 0;
    [[nodiscard]] virtual auto status() const -> result<mount_status> = 0;
};

[[nodiscard]] auto start_fuse(repository& source,
                              std::filesystem::path mountpoint,
                              mount_options options) -> runtime::task<std::shared_ptr<backend>>;
[[nodiscard]] auto start_observed_fuse(repository& source,
                                       digest tree,
                                       std::filesystem::path mountpoint,
                                       std::vector<std::string> outputs,
                                       observation_sink sink,
                                       bool read_only = true)
    -> runtime::task<std::shared_ptr<backend>>;

struct mount_factory {
    [[nodiscard]] static auto make(std::shared_ptr<backend> implementation) -> ::liquid::mount;
};

} // namespace liquid::vfs_detail
