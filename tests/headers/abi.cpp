// SPDX-License-Identifier: MIT

#include "liquid/abi.hpp"

void use_abi_header() {
    liquid::abi_descriptor descriptor;
    liquid::abi_report report;
    static_cast<void>(descriptor);
    static_cast<void>(report);
}
