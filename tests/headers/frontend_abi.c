#include "liquid/frontend_abi.h"

int main(void) {
    if (LIQUID_FRONTEND_ABI_VERSION != 1u) {
        return 1;
    }
    return LIQUIFY_PLUGIN_ABI_VERSION == 1u ? 0 : 1;
}
