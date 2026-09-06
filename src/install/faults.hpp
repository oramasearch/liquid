#pragma once

#include <cstdint>
#include <filesystem>

#include "liquid/export.hpp"

namespace liquid::install_detail {

enum class fault_operation : std::uint8_t {
    staged_file,
    switch_generation,
};

struct fault_event {
    fault_operation operation{};
    const std::filesystem::path& path;
};

class fault_injector {
public:
    virtual ~fault_injector() = default;
    virtual void visit(fault_event event) = 0;
};

LIQUID_EXPORT fault_injector* set_fault_injector(fault_injector* injector) noexcept;
LIQUID_EXPORT void visit_fault(fault_event event);

} // namespace liquid::install_detail
