#include <gtest/gtest.h>

#include "liquid/drip.hpp"
#include "unsupported.hpp"

TEST(Drip, FacadeNamesOwningIssues) {
    static_assert(sizeof(liquid::capture) == sizeof(void*));
    static_assert(sizeof(liquid::subscription) == sizeof(void*));
    liquid::capture value;
    expect_unsupported(value.subscribe([](const liquid::drip&) {}), "liquid #5");
    expect_unsupported(value.pause(), "liquid #48");
    expect_unsupported(value.resume(), "liquid #48");
    expect_unsupported(value.status(), "liquid #48");
}
