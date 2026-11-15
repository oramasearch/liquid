// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>

#include "liquid/export.hpp"

namespace liquid::store_detail {

enum class fault_operation : std::uint8_t { write, rename, remove };
enum class fault_phase : std::uint8_t { before, after };

struct fault_event {
    fault_operation operation{};
    fault_phase phase{};
    const std::filesystem::path& path;
};

class fault_injector {
public:
    virtual ~fault_injector() = default;
    virtual void visit(fault_event event) = 0;
};

// Internal test control. Production leaves the pointer null, making a fault
// point one atomic load and one branch.
LIQUID_EXPORT fault_injector* set_fault_injector(fault_injector* injector) noexcept;
LIQUID_EXPORT void visit_fault(fault_event event);

} // namespace liquid::store_detail
