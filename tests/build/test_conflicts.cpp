#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/materialise.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class conflict_build_repository {
public:
    conflict_build_repository()
        : root_(liquid::test::fixture_path("liquid-conflict-build-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        std::filesystem::create_directories(root_, code);
        EXPECT_FALSE(code) << code.message();
        auto opened = liquid::runtime::block_on(liquid::repository::create(root_ / "repository"));
        EXPECT_TRUE(opened) << opened.error().message;
        source = std::move(*opened);
    }

    ~conflict_build_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    auto output(std::string_view name) const -> std::filesystem::path {
        return root_ / name;
    }

    liquid::repository source;

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path root_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto entry_reference(liquid::repository& source, std::string_view name) -> liquid::digest {
    auto tree = liquid::runtime::block_on(source.root());
    EXPECT_TRUE(tree) << tree.error().message;
    const auto entry = std::ranges::find(tree->entries, name, &liquid::tree_entry::name);
    EXPECT_NE(entry, tree->entries.end());
    return entry->ref;
}

TEST(BuildConflicts, only_a_reached_conflicted_path_stops_materialisation) {
    conflict_build_repository fixture;
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("conflicted.txt", bytes("base"))));
    const auto base = entry_reference(fixture.source, "conflicted.txt");
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("conflicted.txt", bytes("left"))));
    const auto left = entry_reference(fixture.source, "conflicted.txt");
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("conflicted.txt", bytes("right"))));
    const auto right = entry_reference(fixture.source, "conflicted.txt");
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("safe.txt", bytes("available"))));

    auto conflict = liquid::conflict{
        .base = base,
        .left = left,
        .right = right,
        .region = {.path = "conflicted.txt", .bytes = liquid::conflict_byte_range{0, 5}},
        .kind = liquid::conflict_kind::text,
        .explanation = "both sides changed the same bytes"};
    auto conflict_id =
        liquid::runtime::block_on(fixture.source.record_conflict(std::move(conflict)));
    ASSERT_TRUE(conflict_id);
    auto conflicted_root = liquid::runtime::block_on(fixture.source.root_digest());
    ASSERT_TRUE(conflicted_root);

    auto blocked = liquid::runtime::block_on(liquid::build_detail::materialise_tree(
        fixture.source, *conflicted_root, fixture.output("blocked")));
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, liquid::errc::conflict);
    EXPECT_NE(blocked.error().context.find("conflicted.txt"), std::string::npos);
    EXPECT_NE(blocked.error().context.find(liquid::to_string(*conflict_id)), std::string::npos);

    auto safe_tree = liquid::runtime::block_on(fixture.source.tree(*conflicted_root));
    ASSERT_TRUE(safe_tree);
    std::erase_if(safe_tree->entries, [](const liquid::tree_entry& entry) {
        return entry.kind == liquid::node_kind::conflict;
    });
    auto encoded = liquid::objects::encode(*safe_tree);
    ASSERT_TRUE(encoded);
    auto safe_root = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(fixture.source, std::move(*encoded)));
    ASSERT_TRUE(safe_root);
    auto materialised = liquid::runtime::block_on(
        liquid::build_detail::materialise_tree(fixture.source, *safe_root, fixture.output("safe")));
    ASSERT_TRUE(materialised) << materialised.error().message;
    auto input = std::ifstream{fixture.output("safe/safe.txt"), std::ios::binary};
    ASSERT_TRUE(input);
    const auto contents = std::string{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(contents, "available");
}

} // namespace
