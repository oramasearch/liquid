// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../temporary_directory.hpp"
#include "harness.hpp"

namespace {

class regression_workspace {
public:
    regression_workspace() : path_(liquid::test::fixture_path("liquid-gc-regression-seed-140")) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~regression_workspace() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

TEST(CollectorRegression, Seed140KeepsRootsAfterCollectionAndCompactionCrash) {
    regression_workspace workspace;
    const auto outcome = liquid::sim::run_store_simulation(140, workspace.path());
    EXPECT_GT(outcome.fault_step, 0U);
    ASSERT_FALSE(outcome.failure.has_value())
        << (outcome.failure ? outcome.failure->message : std::string{});
}

} // namespace
