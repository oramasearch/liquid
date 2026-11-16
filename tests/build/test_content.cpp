// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <ranges>
#include <string>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/content.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class content_repository {
public:
    content_repository()
        : path_(liquid::test::fixture_path("liquid-content-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        auto opened = liquid::runtime::block_on(liquid::repository::create(path_));
        EXPECT_TRUE(opened) << opened.error().message;
        source = std::move(*opened);
    }

    ~content_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto put_tree(liquid::tree value) -> liquid::digest {
        auto encoded = liquid::objects::encode(value);
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(source, std::move(*encoded)));
        EXPECT_TRUE(stored) << stored.error().message;
        return *stored;
    }

    liquid::repository source;

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto example_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

TEST(ContentAddressedOutputs, named_trees_are_folded_under_read_only_dependencies) {
    content_repository fixture;
    const auto dependency = fixture.put_tree({});
    const auto base = fixture.put_tree({.entries = {{.name = "source.c",
                                                     .mode = 0644,
                                                     .kind = liquid::node_kind::blob,
                                                     .id = example_digest(1),
                                                     .ref = example_digest(1)}}});
    auto folded = liquid::runtime::block_on(liquid::build_detail::fold_dependency_inputs(
        fixture.source, base, std::vector<liquid::named_input>{{"zlib", dependency}}));
    ASSERT_TRUE(folded) << folded.error().message;
    auto root = liquid::runtime::block_on(fixture.source.tree(*folded));
    ASSERT_TRUE(root) << root.error().message;
    const auto dependencies = std::ranges::find(root->entries, "deps", &liquid::tree_entry::name);
    ASSERT_NE(dependencies, root->entries.end());
    EXPECT_EQ(dependencies->mode, 0040555U);
    auto dependency_root = liquid::runtime::block_on(fixture.source.tree(dependencies->ref));
    ASSERT_TRUE(dependency_root) << dependency_root.error().message;
    ASSERT_EQ(dependency_root->entries.size(), 1U);
    EXPECT_EQ(dependency_root->entries.front().name, "zlib");
    EXPECT_EQ(dependency_root->entries.front().mode, 0040555U);
    EXPECT_EQ(liquid::to_string(dependency_root->entries.front().ref),
              liquid::to_string(dependency));
}

TEST(ContentAddressedOutputs, refreshed_inputs_keep_read_only_dependencies) {
    content_repository fixture;
    const auto dependency = fixture.put_tree({});
    const auto recorded_base = fixture.put_tree({.entries = {{.name = "source.c",
                                                              .mode = 0644,
                                                              .kind = liquid::node_kind::blob,
                                                              .id = example_digest(2),
                                                              .ref = example_digest(2)}}});
    auto recorded = liquid::runtime::block_on(liquid::build_detail::fold_dependency_inputs(
        fixture.source, recorded_base, std::vector<liquid::named_input>{{"zlib", dependency}}));
    ASSERT_TRUE(recorded) << recorded.error().message;
    const auto current = fixture.put_tree({.entries = {{.name = "source.c",
                                                        .mode = 0644,
                                                        .kind = liquid::node_kind::blob,
                                                        .id = example_digest(3),
                                                        .ref = example_digest(3)}}});
    auto refreshed = liquid::runtime::block_on(
        liquid::build_detail::refresh_dependency_input_tree(fixture.source, current, *recorded));
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    auto root = liquid::runtime::block_on(fixture.source.tree(*refreshed));
    ASSERT_TRUE(root) << root.error().message;
    const auto source = std::ranges::find(root->entries, "source.c", &liquid::tree_entry::name);
    const auto dependencies = std::ranges::find(root->entries, "deps", &liquid::tree_entry::name);
    ASSERT_NE(source, root->entries.end());
    ASSERT_NE(dependencies, root->entries.end());
    EXPECT_EQ(liquid::to_string(source->ref), liquid::to_string(example_digest(3)));
    EXPECT_EQ(dependencies->mode, 0040555U);
    auto dependency_root = liquid::runtime::block_on(fixture.source.tree(dependencies->ref));
    ASSERT_TRUE(dependency_root) << dependency_root.error().message;
    ASSERT_EQ(dependency_root->entries.size(), 1U);
    EXPECT_EQ(liquid::to_string(dependency_root->entries.front().ref),
              liquid::to_string(dependency));
}

TEST(ContentAddressedOutputs, output_tree_preserves_paths_and_normalises_modes) {
    content_repository fixture;
    const auto outputs = std::vector<liquid::action_output>{
        {"lib/libz.a", 0100664, example_digest(2)}, {"bin/example", 0100770, example_digest(3)}};
    auto stored =
        liquid::runtime::block_on(liquid::build_detail::store_output_tree(fixture.source, outputs));
    ASSERT_TRUE(stored) << stored.error().message;
    auto root = liquid::runtime::block_on(fixture.source.tree(*stored));
    ASSERT_TRUE(root) << root.error().message;
    const auto bin = std::ranges::find(root->entries, "bin", &liquid::tree_entry::name);
    const auto lib = std::ranges::find(root->entries, "lib", &liquid::tree_entry::name);
    ASSERT_NE(bin, root->entries.end());
    ASSERT_NE(lib, root->entries.end());
    auto bin_tree = liquid::runtime::block_on(fixture.source.tree(bin->ref));
    auto lib_tree = liquid::runtime::block_on(fixture.source.tree(lib->ref));
    ASSERT_TRUE(bin_tree);
    ASSERT_TRUE(lib_tree);
    EXPECT_EQ(bin_tree->entries.front().mode, 0100755U);
    EXPECT_EQ(lib_tree->entries.front().mode, 0100644U);
}

} // namespace
