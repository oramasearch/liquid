#pragma once

#include <memory>

#include "liquid/export.hpp"

namespace liquid {

class LIQUID_EXPORT remote {
public:
    remote() noexcept;
    ~remote();
    remote(remote&&) noexcept;
    remote& operator=(remote&&) noexcept;
    remote(const remote&) = delete;
    remote& operator=(const remote&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
