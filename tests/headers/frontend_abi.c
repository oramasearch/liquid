#include "liquid/frontend_abi.h"

int main(void) {
    return LIQUID_FRONTEND_ABI_VERSION == 1u ? 0 : 1;
}
