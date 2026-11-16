// SPDX-License-Identifier: MIT

#include "liquid/hot_abi.h"

int main(void) {
    return LIQUID_HOT_ABI_VERSION == 1u ? 0 : 1;
}
