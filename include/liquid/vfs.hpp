#pragma once

#include <memory>

#include "liquid/export.hpp"

namespace liquid {

class LIQUID_EXPORT mount {
public:
    mount() noexcept;
    ~mount();
    mount(mount&&) noexcept;
    mount& operator=(mount&&) noexcept;
    mount(const mount&) = delete;
    mount& operator=(const mount&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
