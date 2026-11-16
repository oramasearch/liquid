// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>

#include "repository/ref_name.hpp"

// L-38 (liquid#424, liquid#387): a reference name follows git check-ref-format, so a stored name can
// never leave the refs directory a Git export writes it under.
TEST(RefName, FollowsGitCheckRefFormat) {
    using liquid::repository_detail::valid_ref_name;
    for (const std::string good : {"main", "feature/login", "tags/v1.2.3", "refs/heads/main",
                                   "refs/pull/12/head", "release-2026_11"}) {
        EXPECT_TRUE(valid_ref_name(good)) << good;
    }
    for (const std::string bad :
         {"", "@", "../../outside", "refs/../config", "main/", "/main", "a//b", ".hidden",
          "feature/.x", "main.lock", "a..b", "a@{1}", "has space", "tab\there", "ctl\x01",
          "colon:ref", "star*", "q?", "[x", "back\\slash", "tilde~1", "caret^", "end."}) {
        EXPECT_FALSE(valid_ref_name(bad)) << bad;
    }
}
