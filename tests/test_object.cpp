#include <gtest/gtest.h>

#include <type_traits>

#include "liquid/object.hpp"

TEST(Object, RecordsArePlainValues) {
    static_assert(std::is_aggregate_v<liquid::blob>);
    static_assert(std::is_aggregate_v<liquid::tree_entry>);
    static_assert(std::is_aggregate_v<liquid::tree>);
    static_assert(std::is_aggregate_v<liquid::change_id>);
    static_assert(std::is_aggregate_v<liquid::conflict>);
    static_assert(std::is_aggregate_v<liquid::change>);
    EXPECT_EQ(static_cast<unsigned>(liquid::node_kind::repo), 6u);
}
