#include <gtest/gtest.h>

#include <type_traits>

#include "liquid/remote.hpp"

TEST(Remote, RemoteIsAnOpaqueMoveOnlyHandle) {
    static_assert(sizeof(liquid::remote) == sizeof(void*));
    static_assert(std::is_move_constructible_v<liquid::remote>);
    static_assert(!std::is_copy_constructible_v<liquid::remote>);
    liquid::remote value;
    SUCCEED();
}
