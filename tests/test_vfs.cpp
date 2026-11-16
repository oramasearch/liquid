// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <type_traits>

#include "drive/settings.hpp"
#include "liquid/vfs.hpp"
#include "vfs/xattrs.hpp"

TEST(Vfs, MountIsAnOpaqueMoveOnlyHandle) {
    static_assert(sizeof(liquid::mount) == sizeof(void*));
    static_assert(std::is_aggregate_v<liquid::mount_options>);
    static_assert(std::is_aggregate_v<liquid::mount_status>);
    static_assert(std::is_move_constructible_v<liquid::mount>);
    static_assert(!std::is_copy_constructible_v<liquid::mount>);
    liquid::mount value;
    SUCCEED();
}

TEST(VfsDriveState, folder_rules_and_explicit_overrides_resolve_deterministically) {
    auto settings = liquid::drive_detail::stored_settings{};
    settings.states["photos"] = liquid::drive_path_state::available_offline;
    EXPECT_EQ(liquid::drive_detail::effective_state(settings, "photos/2026/image.jpg"),
              liquid::drive_path_state::available_offline);
    settings.states["photos/2026/image.jpg"] = liquid::drive_path_state::online_only;
    EXPECT_EQ(liquid::drive_detail::effective_state(settings, "photos/2026/image.jpg"),
              liquid::drive_path_state::online_only);
    settings.states["photos/private"] = liquid::drive_path_state::not_synced_here;
    EXPECT_EQ(liquid::drive_detail::effective_state(settings, "photos/private/image.jpg"),
              liquid::drive_path_state::not_synced_here);
    settings.policy.pin_folders = false;
    EXPECT_EQ(liquid::drive_detail::effective_state(settings, "photos/other.jpg"),
              liquid::drive_path_state::online_only);
    EXPECT_EQ(liquid::drive_detail::effective_state(settings, "photos/private/image.jpg"),
              liquid::drive_path_state::not_synced_here);

    settings.policy.hydration_size_threshold = 4096;
    EXPECT_TRUE(liquid::drive_detail::should_hydrate_fully(settings, "small.txt", 4096));
    EXPECT_FALSE(liquid::drive_detail::should_hydrate_fully(settings, "large.bin", 4097));
    settings.states["large.bin"] = liquid::drive_path_state::available_offline;
    EXPECT_TRUE(liquid::drive_detail::should_hydrate_fully(settings, "large.bin", 4097));
}

TEST(VfsDriveXattr, exposes_and_accepts_the_three_state_contract) {
    using liquid::drive_path_state;
    using liquid::vfs_detail::drive_xattr_state;
    using liquid::vfs_detail::drive_xattr_value;
    EXPECT_EQ(*drive_xattr_value(drive_path_state::online_only, "user.liquid.state"),
              "online-only");
    EXPECT_EQ(*drive_xattr_value(drive_path_state::available_offline, "user.liquid.state"),
              "available-offline");
    EXPECT_EQ(*drive_xattr_value(drive_path_state::not_synced_here, "user.liquid.state"),
              "not-synced-here");
    EXPECT_EQ(*drive_xattr_value(drive_path_state::available_offline, "user.liquid.pin"), "1");
    EXPECT_EQ(*drive_xattr_value(drive_path_state::online_only, "user.liquid.pin"), "0");
    EXPECT_EQ(*drive_xattr_state("user.liquid.state", "not-synced-here"),
              drive_path_state::not_synced_here);
    EXPECT_EQ(*drive_xattr_state("user.liquid.pin", "1"), drive_path_state::available_offline);
    EXPECT_FALSE(drive_xattr_state("user.liquid.pin", "yes"));
    EXPECT_FALSE(drive_xattr_value(drive_path_state::online_only, "user.unknown"));
    const auto names = std::string{"user.liquid.state\0user.liquid.pin\0", 34};
    EXPECT_EQ(liquid::vfs_detail::drive_xattr_names(), names);
}
