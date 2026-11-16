// SPDX-License-Identifier: MIT

#include "liquid/frontend_abi.h"

#include <stddef.h>
#include <stdint.h>

typedef struct plugin_state {
    size_t counts[3];
    uint8_t last_values[3];
} plugin_state;

static plugin_state state;

static void receive_presence(void* context, liquid_frontend_bytes_v1 event_cbor) {
    plugin_state* plugin = context;
    plugin->counts[0] += 1;
    plugin->last_values[0] = event_cbor.size == 0 ? 0 : event_cbor.data[0];
}

static void receive_lease(void* context, liquid_frontend_bytes_v1 event_cbor) {
    plugin_state* plugin = context;
    plugin->counts[1] += 1;
    plugin->last_values[1] = event_cbor.size == 0 ? 0 : event_cbor.data[0];
}

static void receive_attribution(void* context, liquid_frontend_bytes_v1 event_cbor) {
    plugin_state* plugin = context;
    plugin->counts[2] += 1;
    plugin->last_values[2] = event_cbor.size == 0 ? 0 : event_cbor.data[0];
}

static const liquify_plugin_event_callbacks_v1 events = {
    LIQUIFY_PLUGIN_EVENTS_ABI_VERSION,
    sizeof(liquify_plugin_event_callbacks_v1),
    receive_presence,
    receive_lease,
    receive_attribution,
};

static const liquify_plugin_v1 plugin = {
    LIQUIFY_PLUGIN_ABI_VERSION,
    sizeof(liquify_plugin_v1),
    "event-test-plugin",
    NULL,
    0,
    NULL,
    0,
    NULL,
    0,
    &state,
    NULL,
    &events,
};

const liquify_plugin_v1* liquify_plugin_get_v1(void) {
    return &plugin;
}

void liquify_plugin_test_reset(void) {
    state = (plugin_state){{0, 0, 0}, {0, 0, 0}};
}

size_t liquify_plugin_test_count(size_t event) {
    return event < 3 ? state.counts[event] : 0;
}

uint8_t liquify_plugin_test_last_value(size_t event) {
    return event < 3 ? state.last_values[event] : 0;
}
