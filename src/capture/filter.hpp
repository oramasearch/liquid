#pragma once

#include <vector>

#include "capture/watcher.hpp"

namespace liquid::capture_detail {

class drip_filter {
  public:
    virtual ~drip_filter() = default;
    [[nodiscard]] virtual auto accept(const std::vector<watch_event> &events) const -> result<bool> = 0;
};

class allow_all_filter final : public drip_filter {
  public:
    [[nodiscard]] auto accept(const std::vector<watch_event> &) const -> result<bool> override {
        return true;
    }
};

} // namespace liquid::capture_detail
