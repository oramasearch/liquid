#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <optional>

#include "liquid/repository.hpp"
#include "unsupported.hpp"

TEST(Repository, FacadeNamesOwningIssues) {
    static_assert(sizeof(liquid::repository) == sizeof(void*));
    const std::filesystem::path path{"repo"};
    expect_unsupported(liquid::repository::create(path), "liquid #2");
    expect_unsupported(liquid::repository::open(path), "liquid #2");

    liquid::repository value;
    liquid::repository other;
    const liquid::digest digest{liquid::algorithm::blake3, {}};
    const liquid::change_id change{digest};
    const liquid::op_id operation{digest};
    std::array<std::byte, 1> contents{};
    expect_unsupported(value.root(), "liquid #2");
    expect_unsupported(value.head(), "liquid #2");
    expect_unsupported(value.read("file"), "liquid #2");
    expect_unsupported(value.write("file", contents), "liquid #2");
    expect_unsupported(value.log(), "liquid #2");
    expect_unsupported(value.at(change), "liquid #2");
    expect_unsupported(value.nest(other, "nested"), "liquid #2");
    expect_unsupported(value.split("nested", path), "liquid #2");
    expect_unsupported(value.combine(other, std::nullopt), "liquid #2");
    expect_unsupported(value.conflicts(), "liquid #40");
    expect_unsupported(value.undo(operation), "liquid #41");
    expect_unsupported(value.lock("file"), "liquid #42");
    expect_unsupported(value.unlock("file"), "liquid #42");
}
