// SPDX-License-Identifier: MIT

#include "liquid/format_abi.h"

int main(void) {
    liquid_format_module_v1 module = {0};
    module.abi_version = LIQUID_FORMAT_ABI_VERSION;
    return module.abi_version == 1u ? 0 : 1;
}
