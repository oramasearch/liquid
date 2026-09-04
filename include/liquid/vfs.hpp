#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT mount_options {
    bool read_only{false};
    std::chrono::seconds attr_timeout{1};
};

struct LIQUID_EXPORT mount_status {
    std::uint64_t hydrated_bytes{};
    std::uint64_t hydrated_files{};
    std::uint64_t misses_served{};
    std::uint64_t writes_captured{};
    std::chrono::nanoseconds request_queue_latency{};
};

namespace vfs_detail {
struct mount_factory;
}

class LIQUID_EXPORT mount {
public:
    mount() noexcept;
    ~mount();
    mount(mount&&) noexcept;
    mount& operator=(mount&&) noexcept;
    mount(const mount&) = delete;
    mount& operator=(const mount&) = delete;

    [[nodiscard]] runtime::task<void> unmount();
    [[nodiscard]] result<mount_status> status() const;

private:
    friend struct vfs_detail::mount_factory;
    struct impl;
    std::unique_ptr<impl> impl_;
};

namespace vfs {

[[nodiscard]] LIQUID_EXPORT runtime::task<::liquid::mount>
mount(repository& source, std::filesystem::path mountpoint, mount_options options = {});

[[nodiscard]] LIQUID_EXPORT runtime::task<void>
materialise(repository& source,
            std::filesystem::path target,
            std::optional<std::string> path_prefix = std::nullopt);

} // namespace vfs

} // namespace liquid
