// SPDX-License-Identifier: MIT

#pragma once

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/watcher.hpp"
#include "scan/detectors.hpp"

namespace liquid::capture_detail {

class drip_filter {
public:
    virtual ~drip_filter() = default;
    [[nodiscard]] virtual auto accept(const std::vector<watch_event>& events) const
        -> result<bool> = 0;
};

class allow_all_filter final : public drip_filter {
public:
    [[nodiscard]] auto accept(const std::vector<watch_event>&) const -> result<bool> override {
        return true;
    }
};

class secret_filter {
public:
    explicit secret_filter(scan_configuration config = {}) : config_(std::move(config)) {}

    [[nodiscard]] auto
    inspect(std::string_view path,
            std::span<const std::byte> contents,
            std::optional<std::span<const scan_detail::byte_range>> changed = std::nullopt) const
        -> scan_detail::report {
        return detectors_.scan(path, contents, config_, changed);
    }

private:
    scan_configuration config_;
    scan_detail::detector_set detectors_;
};

} // namespace liquid::capture_detail
