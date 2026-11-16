// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "liquid/frontend_abi.h"

extern "C" {
auto liquify_plugin_get_v1() -> const liquify_plugin_v1*;
void liquify_plugin_test_reset();
auto liquify_plugin_test_count(std::size_t event) -> std::size_t;
auto liquify_plugin_test_last_value(std::size_t event) -> std::uint8_t;
}

TEST(PluginAbi, versioned_event_table_receives_presence_lease_and_attribution) {
    liquify_plugin_test_reset();
    const auto* plugin = liquify_plugin_get_v1();
    ASSERT_NE(plugin, nullptr);
    ASSERT_GE(plugin->struct_size,
              offsetof(liquify_plugin_v1, events) + sizeof(liquify_plugin_v1::events));
    ASSERT_NE(plugin->events, nullptr);
    EXPECT_EQ(plugin->events->abi_version, LIQUIFY_PLUGIN_EVENTS_ABI_VERSION);
    EXPECT_EQ(plugin->events->struct_size, sizeof(liquify_plugin_event_callbacks_v1));

    const auto payloads = std::array<std::array<std::uint8_t, 4>, 3>{
        {{{0xa1, 0x61, 0x76, 0x01}}, {{0xa1, 0x61, 0x76, 0x01}}, {{0xa1, 0x61, 0x76, 0x01}}}};
    plugin->events->presence(plugin->plugin_context, {payloads[0].data(), payloads[0].size()});
    plugin->events->lease(plugin->plugin_context, {payloads[1].data(), payloads[1].size()});
    plugin->events->attribution(plugin->plugin_context, {payloads[2].data(), payloads[2].size()});

    for (auto index = std::size_t{}; index < payloads.size(); ++index) {
        EXPECT_EQ(liquify_plugin_test_count(index), 1U);
        EXPECT_EQ(liquify_plugin_test_last_value(index), payloads[index][0]);
    }
}

TEST(PluginAbi, event_subscription_is_an_optional_descriptor_tail) {
    EXPECT_EQ(offsetof(liquify_plugin_v1, events),
              offsetof(liquify_plugin_v1, destroy) + sizeof(liquify_plugin_v1::destroy));
}
