// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "repository/store_access.hpp"
#include "repository/sync_access.hpp"
#include "repository/testing.hpp"
#include "store/references.hpp"
#include "temporary_directory.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class merge_fixture {
public:
    merge_fixture()
        : root_(liquid::test::fixture_path("liquid-structural-merge-" +
                                           std::to_string(sequence_++))) {
        std::filesystem::remove_all(root_);
    }

    ~merge_fixture() {
        std::filesystem::remove_all(root_);
    }

    auto path(std::string_view name) const -> std::filesystem::path {
        return root_ / name;
    }

private:
    inline static std::atomic_uint64_t sequence_{};
    std::filesystem::path root_;
};

struct exported_repository {
    std::vector<liquid::sync_item> items;
    std::vector<liquid::digest> roots;
};

auto export_all(liquid::repository& source) -> liquid::result<exported_repository> {
    auto cursor = liquid::journal_cursor{};
    auto output = exported_repository{};
    for (;;) {
        auto page = std::vector<liquid::sync_item>(32);
        auto exported = liquid::runtime::block_on(source.export_drips(cursor, page));
        if (!exported) {
            return std::unexpected(exported.error());
        }
        output.items.insert(output.items.end(), page.begin(), page.begin() + exported->count);
        output.roots.insert(output.roots.end(), exported->objects.begin(), exported->objects.end());
        cursor = exported->cursor;
        if (exported->done) {
            return output;
        }
    }
}

auto copy_graph(liquid::repository& destination,
                liquid::repository& source,
                std::span<const liquid::digest> roots) -> liquid::result<void> {
    auto& source_store = liquid::repository_detail::sync_access::objects(source);
    auto& destination_store = liquid::repository_detail::sync_access::objects(destination);
    auto pending = std::deque<liquid::digest>{roots.begin(), roots.end()};
    auto visited = std::set<std::string>{};
    auto registry = liquid::store_detail::reference_registry{};
    while (!pending.empty()) {
        const auto id = pending.front();
        pending.pop_front();
        if (!visited.insert(liquid::to_string(id)).second) {
            continue;
        }
        auto encoded = liquid::runtime::block_on(source_store.get(id));
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto kind = liquid::store_detail::detect_object_kind(*encoded);
        const auto references = registry.extract(kind, *encoded);
        pending.insert(pending.end(), references.digests.begin(), references.digests.end());
        auto stored =
            liquid::runtime::block_on(liquid::repository_detail::store_access::put_verified(
                destination_store, id, std::move(*encoded)));
        if (!stored) {
            return std::unexpected(stored.error());
        }
    }
    return {};
}

struct replicas {
    liquid::repository left;
    liquid::repository right;
};

auto make_replicas(merge_fixture& fixture, std::string_view initial) -> liquid::result<replicas> {
    auto source = liquid::runtime::block_on(liquid::repository::create(fixture.path("source")));
    if (!source) {
        return std::unexpected(source.error());
    }
    auto written = liquid::runtime::block_on(source->write("functions.cpp", bytes(initial)));
    if (!written) {
        return std::unexpected(written.error());
    }
    auto cut = liquid::runtime::block_on(source->cut("base"));
    if (!cut) {
        return std::unexpected(cut.error());
    }
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("left")));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("right")));
    if (!left || !right) {
        return std::unexpected(!left ? left.error() : right.error());
    }
    return replicas{std::move(*left), std::move(*right)};
}

auto sync_from(liquid::repository& destination, liquid::repository& source)
    -> liquid::result<void> {
    auto items = export_all(source);
    if (!items) {
        return std::unexpected(items.error());
    }
    auto copied = copy_graph(destination, source, items->roots);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto imported = liquid::runtime::block_on(destination.import_drips(items->items));
    if (!imported) {
        return std::unexpected(imported.error());
    }
    return {};
}

TEST(StructuralMerge, disjoint_function_additions_merge_and_record_inputs) {
    merge_fixture fixture;
    auto pair = make_replicas(fixture, "int base() { return 0; }\n");
    ASSERT_TRUE(pair) << pair.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(pair->left.write(
        "functions.cpp", bytes("int base() { return 0; }\nint left_only() { return 1; }\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(pair->right.write(
        "functions.cpp", bytes("int base() { return 0; }\nint right_only() { return 2; }\n"))));
    auto synchronized = sync_from(pair->left, pair->right);
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    auto merged = liquid::runtime::block_on(pair->left.read("functions.cpp"));
    ASSERT_TRUE(merged);
    EXPECT_NE(text(*merged).find("left_only"), std::string::npos);
    EXPECT_NE(text(*merged).find("right_only"), std::string::npos);
    auto conflicts = liquid::runtime::block_on(pair->left.conflicts());
    ASSERT_TRUE(conflicts);
    EXPECT_TRUE(conflicts->empty());
    auto operations = liquid::runtime::block_on(pair->left.operations());
    ASSERT_TRUE(operations);
    ASSERT_FALSE(operations->empty());
    const auto merge = std::ranges::find(*operations, "merge", &liquid::operation_record::kind);
    ASSERT_NE(merge, operations->end());
    EXPECT_EQ(merge->metadata.at("algorithm"), "structural-three-way-v1");
    EXPECT_FALSE(merge->metadata.at("grammar").empty());
    EXPECT_TRUE(merge->metadata.contains("base"));
    EXPECT_TRUE(merge->metadata.contains("left"));
    EXPECT_TRUE(merge->metadata.contains("right"));
}

TEST(StructuralMerge, same_name_additions_are_suspicious_and_accepting_proposal_clears_them) {
    merge_fixture fixture;
    auto pair = make_replicas(fixture, "int base() { return 0; }\n");
    ASSERT_TRUE(pair) << pair.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(pair->left.write(
        "functions.cpp", bytes("int base() { return 0; }\nint duplicate() { return 1; }\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(pair->right.write(
        "functions.cpp", bytes("int base() { return 0; }\nint duplicate() { return 2; }\n"))));
    auto synchronized = sync_from(pair->left, pair->right);
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    auto conflicts = liquid::runtime::block_on(pair->left.conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::suspicious_merge);
    ASSERT_TRUE(conflicts->front().proposal);
    EXPECT_FALSE(conflicts->front().reasons.empty());
    const auto conflict_id = conflicts->front().id;
    auto recorded = liquid::runtime::block_on(pair->left.cut("record suspicious merge"));
    ASSERT_TRUE(recorded);
    auto peer = liquid::runtime::block_on(liquid::repository::create(fixture.path("peer")));
    ASSERT_TRUE(peer);
    auto round_trip = sync_from(*peer, pair->left);
    ASSERT_TRUE(round_trip) << round_trip.error().message << ": " << round_trip.error().context;
    auto peer_conflicts = liquid::runtime::block_on(peer->conflicts());
    ASSERT_TRUE(peer_conflicts);
    ASSERT_EQ(peer_conflicts->size(), 1U);
    EXPECT_EQ(peer_conflicts->front().kind, liquid::conflict_kind::suspicious_merge);
    ASSERT_TRUE(peer_conflicts->front().proposal);
    EXPECT_EQ(liquid::to_string(*peer_conflicts->front().proposal),
              liquid::to_string(*conflicts->front().proposal));
    auto resolved = liquid::runtime::block_on(pair->left.resolve_conflict(
        conflict_id, {.kind = liquid::conflict_resolution_kind::accept_proposal}));
    ASSERT_TRUE(resolved) << resolved.error().message;
    conflicts = liquid::runtime::block_on(pair->left.conflicts());
    ASSERT_TRUE(conflicts);
    EXPECT_TRUE(conflicts->empty());
    auto history = liquid::runtime::block_on(pair->left.log());
    ASSERT_TRUE(history);
    ASSERT_FALSE(history->empty());
    EXPECT_TRUE(history->front().conflicts.empty());
    ASSERT_EQ(history->front().resolutions.size(), 1U);
}

TEST(StructuralMerge, broken_structural_proposals_remain_conflict_values) {
    merge_fixture fixture;
    auto pair = make_replicas(fixture, "int base() { return 0; }\n");
    ASSERT_TRUE(pair) << pair.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        pair->left.write("functions.cpp", bytes("int base() { return 1; }\n@@@@\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(pair->right.write(
        "functions.cpp", bytes("int base() { return 2; }\nint right_only() { return 2; }\n"))));
    auto synchronized = sync_from(pair->left, pair->right);
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    auto conflicts = liquid::runtime::block_on(pair->left.conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::suspicious_merge);
    ASSERT_TRUE(conflicts->front().proposal);
    EXPECT_TRUE(std::ranges::any_of(conflicts->front().reasons, [](const auto& reason) {
        return reason.find("ERROR or MISSING") != std::string::npos;
    }));
    auto selected = liquid::runtime::block_on(pair->left.read("functions.cpp"));
    ASSERT_TRUE(selected);
    const auto selected_text = text(*selected);
    EXPECT_FALSE(selected_text.contains("return 1;") && selected_text.contains("right_only"));
}

TEST(StructuralMerge, grammarless_files_keep_text_conflicts) {
    merge_fixture fixture;
    auto source = liquid::runtime::block_on(liquid::repository::create(fixture.path("source")));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("notes.unknown", bytes("base\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("left")));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("right")));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("notes.unknown", bytes("left\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("notes.unknown", bytes("right\n"))));
    auto synchronized = sync_from(*left, *right);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    auto conflicts = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::text);
    EXPECT_NE(conflicts->front().explanation.find("text"), std::string::npos);
    auto operations = liquid::runtime::block_on(left->operations());
    ASSERT_TRUE(operations);
    const auto merge = std::ranges::find(*operations, "merge", &liquid::operation_record::kind);
    ASSERT_NE(merge, operations->end());
    EXPECT_EQ(merge->metadata.at("algorithm"), "entry-three-way-v1");
    EXPECT_EQ(merge->metadata.at("grammar"), "text");
}

TEST(StructuralMerge, grammars_without_definition_units_use_text_conflicts) {
    merge_fixture fixture;
    auto source = liquid::runtime::block_on(liquid::repository::create(fixture.path("source")));
    ASSERT_TRUE(source);
    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("CMakeLists.txt", bytes("project(base)\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("left")));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, fixture.path("right")));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("CMakeLists.txt", bytes("project(left)\n"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(right->write("CMakeLists.txt", bytes("project(right)\n"))));
    auto synchronized = sync_from(*left, *right);
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    auto conflicts = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::text);
    EXPECT_NE(conflicts->front().explanation.find("text structural merge fallback"),
              std::string::npos);
}

} // namespace
