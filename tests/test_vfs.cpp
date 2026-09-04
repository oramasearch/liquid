#include <gtest/gtest.h>

#include <type_traits>

#include "liquid/vfs.hpp"

TEST(Vfs, MountIsAnOpaqueMoveOnlyHandle) {
    static_assert(sizeof(liquid::mount) == sizeof(void*));
    static_assert(std::is_aggregate_v<liquid::mount_options>);
    static_assert(std::is_aggregate_v<liquid::mount_status>);
    static_assert(std::is_move_constructible_v<liquid::mount>);
    static_assert(!std::is_copy_constructible_v<liquid::mount>);
    liquid::mount value;
    SUCCEED();
}
