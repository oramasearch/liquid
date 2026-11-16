// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "../temporary_directory.hpp"

// Exercise the production request handlers without /dev/fuse or a kernel mount.
// Including the implementation keeps its private backend out of the library API.
#include "../../src/vfs/fuse_fs.cpp"

namespace {

using liquid::runtime::block_on;
using namespace liquid::vfs_detail;

struct protected_path {
    bool read_only;
    bool observed;
    std::string path;
};

class FuseXattrRequests : public testing::TestWithParam<protected_path> {
protected:
    liquid::test::detail::process_fixture_directory root;
    liquid::repository repository;
    std::unique_ptr<fuse_backend> state;
    std::vector<observed_request> observations;
    liquid::drive_detail::stored_settings settings;
    static constexpr std::int64_t pid = 12345;

    void SetUp() override {
        auto created = block_on(liquid::repository::create(root.path() / "repository"));
        ASSERT_TRUE(created) << created.error().message;
        repository = std::move(*created);
        const auto content = std::vector<std::byte>{std::byte{'x'}};
        ASSERT_TRUE(block_on(repository.write("input/file", content)));
        ASSERT_TRUE(block_on(repository.write("deps/file", content)));
        auto tree = block_on(repository.root_digest());
        ASSERT_TRUE(tree) << tree.error().message;
        const auto& parameters = GetParam();
        state = std::make_unique<fuse_backend>(
            repository,
            root.path() / "mount",
            root.path() / "repository",
            liquid::mount_options{.read_only = parameters.read_only},
            [this](observed_request request) { observations.push_back(std::move(request)); },
            std::vector<std::string>{},
            parameters.observed ? std::optional{*tree} : std::nullopt);
        ASSERT_TRUE(block_on(state->initialise_cache()));
        settings.policy.pin_folders = true;
        settings.states[parameters.path] = liquid::drive_path_state::available_offline;
        settings.hydrated.insert(parameters.path);
        settings.offline_reason = "preserve existing status";
        ASSERT_TRUE(block_on(liquid::drive_detail::save_settings(repository, settings)));
    }

    template <class Request> void expect_refused(Request request) {
        const auto before =
            block_on(repository.metadata(liquid::drive_detail::settings_metadata_key));
        ASSERT_TRUE(before);
        observations.clear();
        auto result = block_on(request());
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(*result, -EROFS);
        const auto after =
            block_on(repository.metadata(liquid::drive_detail::settings_metadata_key));
        ASSERT_TRUE(after);
        EXPECT_EQ(*after, *before) << "refused xattr must not persist any Drive setting";
        EXPECT_EQ(observations.size(), 1);
        if (observations.size() == 1) {
            EXPECT_EQ(observations.front().operation, observed_operation::write);
            EXPECT_EQ(observations.front().path, GetParam().path);
            EXPECT_EQ(observations.front().pid, pid);
            EXPECT_TRUE(observations.front().denied);
        }
        // Restore the seed so every attempted operation also fails independently on the base.
        ASSERT_TRUE(block_on(liquid::drive_detail::save_settings(repository, settings)));
    }
};

TEST_P(FuseXattrRequests, SetRefusesPolicyChangesAndHydration) {
    for (const auto& [name, value] : std::vector<std::pair<std::string_view, std::string_view>>{
             {state_xattr, "not-synced-here"},
             {state_xattr, "available-offline"},
             {state_xattr, "online-only"},
             {pin_xattr, "1"},
             {pin_xattr, "0"},
             {pin_xattr, "invalid"},
             {"user.unknown", "1"}}) {
        SCOPED_TRACE(std::string{name} + "=" + std::string{value});
        expect_refused([&] {
            return setxattr_request(
                state.get(), GetParam().path, pid, std::string{name}, std::string{value});
        });
    }
}

TEST_P(FuseXattrRequests, RemoveRefusesPolicyReset) {
    for (const auto name : {state_xattr, pin_xattr, std::string_view{"user.unknown"}}) {
        SCOPED_TRACE(name);
        expect_refused([&] {
            return removexattr_request(state.get(), GetParam().path, pid, std::string{name});
        });
    }
}

INSTANTIATE_TEST_SUITE_P(ProtectedMounts,
                         FuseXattrRequests,
                         testing::Values(protected_path{true, false, ""},
                                         protected_path{true, false, "input"},
                                         protected_path{true, false, "input/file"},
                                         protected_path{true, true, ""},
                                         protected_path{true, true, "input"},
                                         protected_path{true, true, "input/file"},
                                         protected_path{false, false, "deps"},
                                         protected_path{false, false, "deps/file"},
                                         protected_path{false, true, "deps"},
                                         protected_path{false, true, "deps/file"}));

TEST(FuseXattrWritable, PersistsSetAndRemoveForBothNames) {
    liquid::test::detail::process_fixture_directory root;
    auto created = block_on(liquid::repository::create(root.path() / "repository"));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(block_on(repository.write("input", {std::byte{'x'}})));
    auto state = fuse_backend{repository,
                              root.path() / "mount",
                              root.path() / "repository",
                              liquid::mount_options{.read_only = false}};
    ASSERT_TRUE(block_on(state.initialise_cache()));
    for (const auto name : {state_xattr, pin_xattr}) {
        const auto value = name == state_xattr ? "available-offline" : "1";
        auto set = block_on(setxattr_request(&state, "input", 12345, std::string{name}, value));
        ASSERT_TRUE(set);
        ASSERT_EQ(*set, 0);
        auto pinned = block_on(liquid::drive_detail::load_settings(repository));
        ASSERT_TRUE(pinned);
        EXPECT_EQ(pinned->states.at("input"), liquid::drive_path_state::available_offline);
        EXPECT_TRUE(pinned->hydrated.contains("input"));
        auto remove = block_on(removexattr_request(&state, "input", 12345, std::string{name}));
        ASSERT_TRUE(remove);
        ASSERT_EQ(*remove, 0);
        auto unpinned = block_on(liquid::drive_detail::load_settings(repository));
        ASSERT_TRUE(unpinned);
        EXPECT_EQ(unpinned->states.at("input"), liquid::drive_path_state::online_only);
    }
}

} // namespace
