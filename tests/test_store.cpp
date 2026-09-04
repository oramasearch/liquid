#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>

#include "liquid/store.hpp"
#include "unsupported.hpp"

TEST(Store, FacadeNamesOwningIssues) {
    static_assert(sizeof(liquid::store) == sizeof(void*));
    static_assert(sizeof(liquid::lease) == sizeof(void*));
    expect_unsupported(liquid::store::open(std::filesystem::path{"store"}), "liquid #1");

    liquid::store value;
    const liquid::digest id{liquid::algorithm::blake3, {}};
    std::array<std::byte, 1> buffer{};
    expect_unsupported(value.put({buffer.begin(), buffer.end()}), "liquid #1");
    expect_unsupported(value.get(id), "liquid #1");
    expect_unsupported(value.read(id, 0, buffer), "liquid #1");
    expect_unsupported(value.contains(id), "liquid #1");
    expect_unsupported(value.lease_root(id, std::chrono::seconds{1}), "liquid #8");
    expect_unsupported(value.collect(), "liquid #8");
}
