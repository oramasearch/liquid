#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "liquid/repository.hpp"
#include "repository/testing.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-repository-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    ~temporary_repository() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto content(std::string value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

TEST(Repository, create_write_move_cut_open_and_time_travel) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("one.txt", content("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("dir/two.txt", content("two"))));
    auto before = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(before);
    ASSERT_EQ(before->entries.size(), 2U);
    const auto identity = before->entries.front().node;
    auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    auto verified = liquid::repository_detail::testing::verify_history(repository);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(*verified, 3U);
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("one.txt", "renamed.txt")));
    auto after = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(after);
    ASSERT_EQ(after->entries.front().node, identity);
    auto second = liquid::runtime::block_on(repository.cut("rename"));
    ASSERT_TRUE(second);
    auto snapshot = liquid::runtime::block_on(repository.at(*first));
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(std::ranges::any_of(snapshot->root.entries,
                                    [](const auto& entry) { return entry.name == "one.txt"; }));
    auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs);
    ASSERT_EQ(refs->size(), 1U);
    EXPECT_EQ(refs->front().id.value.bytes, second->value.bytes);

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("renamed.txt"));
    ASSERT_TRUE(value);
    EXPECT_EQ(text(*value), "one");
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_REPO"));
}

TEST(Repository, write_undo_and_locks_are_recorded) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    auto operation_digest =
        liquid::hash(liquid::algorithm::blake3, root->entries.front().ref.bytes);
    ASSERT_TRUE(operation_digest);
    ASSERT_TRUE(liquid::runtime::block_on(repository.undo({*operation_digest})));
    auto missing = liquid::runtime::block_on(repository.read("file"));
    EXPECT_FALSE(missing);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("file")));
    auto refused = liquid::runtime::block_on(repository.write("file", content("replacement")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    ASSERT_TRUE(liquid::runtime::block_on(repository.unlock("file")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("replacement"))));
}

TEST(Repository, journal_replays_a_write_before_the_next_checkpoint) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("pending.txt", content("journal"))));
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_JOURNAL"));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("pending.txt"));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(text(*value), "journal");
}

TEST(Repository, split_nest_and_combine_are_composable) {
    temporary_repository parent_path;
    temporary_repository child_path;
    temporary_repository other_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("component"))));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(parent->nest(std::move(*split), "nested")));
    auto other = liquid::runtime::block_on(liquid::repository::create(other_path.path()));
    ASSERT_TRUE(other);
    ASSERT_TRUE(liquid::runtime::block_on(other->write("other", content("other"))));
    ASSERT_TRUE(liquid::runtime::block_on(parent->combine(std::move(*other), "other-repository")));
    auto root = liquid::runtime::block_on(parent->root());
    ASSERT_TRUE(root);
    EXPECT_EQ(root->entries.size(), 3U);
}

TEST(Repository, combine_collision_is_a_conflict_value) {
    temporary_repository left_path;
    temporary_repository right_path;
    auto left = liquid::runtime::block_on(liquid::repository::create(left_path.path()));
    auto right = liquid::runtime::block_on(liquid::repository::create(right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("same", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("same", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(left->combine(std::move(*right), "same")));
    auto conflicts = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().path, "same");
    EXPECT_EQ(conflicts->front().refs.size(), 2U);
}

TEST(Repository, cut_loads_only_the_changed_directory_document) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/c/first", content("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/d/second", content("second"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("initial tree")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    liquid::repository_detail::testing::reset_directory_loads(*reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->write("a/b/c/first", content("updated"))));
    EXPECT_EQ(liquid::repository_detail::testing::directory_loads(*reopened), 1U);
}

TEST(Repository, undo_move_restores_the_original_node_identity) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("before", content("value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    const auto identity = root->entries.front().node;
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("before", "after")));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.undo(liquid::repository_detail::testing::last_operation(repository))));
    auto restored = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->entries.size(), 1U);
    EXPECT_EQ(restored->entries.front().name, "before");
    EXPECT_EQ(restored->entries.front().node, identity);
}

TEST(Repository, cross_directory_move_is_recovered_before_cut) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("from/file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("to/anchor", content("anchor"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("from/file", "to/file")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->cut("recover cross-directory move")));
    auto moved = liquid::runtime::block_on(reopened->read("to/file"));
    ASSERT_TRUE(moved);
    EXPECT_EQ(text(*moved), "value");
}

TEST(Repository, synchronized_replicas_turn_divergent_writes_into_a_conflict_value) {
    temporary_repository source_path;
    temporary_repository left_path;
    temporary_repository right_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("a.txt", content("base"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, left_path.path()));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("a.txt", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(liquid::repository_detail::testing::sync(*left, *right)));

    auto left_conflicts = liquid::runtime::block_on(left->conflicts());
    auto right_conflicts = liquid::runtime::block_on(right->conflicts());
    ASSERT_TRUE(left_conflicts);
    ASSERT_TRUE(right_conflicts);
    ASSERT_EQ(left_conflicts->size(), 1U);
    ASSERT_EQ(right_conflicts->size(), 1U);
    EXPECT_EQ(left_conflicts->front().path, "a.txt");
    EXPECT_EQ(left_conflicts->front().refs.size(), 3U);
    ASSERT_EQ(left_conflicts->front().refs.size(), right_conflicts->front().refs.size());
    for (std::size_t index = 0; index < left_conflicts->front().refs.size(); ++index) {
        EXPECT_EQ(left_conflicts->front().refs[index].alg,
                  right_conflicts->front().refs[index].alg);
        EXPECT_EQ(left_conflicts->front().refs[index].bytes,
                  right_conflicts->front().refs[index].bytes);
    }

    auto cut = liquid::runtime::block_on(left->cut("record conflict"));
    ASSERT_TRUE(cut);
    auto history = liquid::runtime::block_on(left->log());
    ASSERT_TRUE(history);
    ASSERT_FALSE(history->empty());
    EXPECT_EQ(history->front().conflicts.size(), 1U);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("resolved"))));
    ASSERT_TRUE(liquid::runtime::block_on(left->cut("resolve conflict")));
    auto resolved = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(resolved);
    EXPECT_TRUE(resolved->empty());
}

} // namespace
