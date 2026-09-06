#include "liquid/frontend_abi.h"

int main(void) {
    liquify_plugin_event_callbacks_v1 events = {
        LIQUIFY_PLUGIN_EVENTS_ABI_VERSION,
        sizeof(liquify_plugin_event_callbacks_v1),
        0,
        0,
        0,
    };
    if (LIQUID_FRONTEND_ABI_VERSION != 1u) {
        return 1;
    }
    return LIQUIFY_PLUGIN_ABI_VERSION == 1u && events.abi_version == 1u ? 0 : 1;
}
