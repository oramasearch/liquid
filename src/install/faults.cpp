// SPDX-License-Identifier: MIT

#include "install/faults.hpp"

#include <atomic>

namespace liquid::install_detail {
namespace {

std::atomic<fault_injector*> active_injector{};

} // namespace

fault_injector* set_fault_injector(fault_injector* injector) noexcept {
    return active_injector.exchange(injector, std::memory_order_acq_rel);
}

void visit_fault(fault_event event) {
    if (auto* injector = active_injector.load(std::memory_order_acquire)) {
        injector->visit(event);
    }
}

} // namespace liquid::install_detail
