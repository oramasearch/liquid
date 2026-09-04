#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "liquid/repository.hpp"

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
  auto path() const -> const std::filesystem::path & { return path_; }

private:
  inline static std::atomic_uint64_t next_{};
  std::filesystem::path path_;
};

auto content(std::string value) -> std::vector<std::byte> {
  return {reinterpret_cast<const std::byte *>(value.data()),
          reinterpret_cast<const std::byte *>(value.data() + value.size())};
}

auto text(const std::vector<std::byte> &value) -> std::string {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
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
  ASSERT_TRUE(liquid::runtime::block_on(repository.move("one.txt", "renamed.txt")));
  auto after = liquid::runtime::block_on(repository.root());
  ASSERT_TRUE(after);
  ASSERT_EQ(after->entries.front().node, identity);
  auto second = liquid::runtime::block_on(repository.cut("rename"));
  ASSERT_TRUE(second);
  auto snapshot = liquid::runtime::block_on(repository.at(*first));
  ASSERT_TRUE(snapshot);
  EXPECT_TRUE(std::ranges::any_of(snapshot->root.entries,
                                  [](const auto &entry) { return entry.name == "one.txt"; }));
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
  auto operation_digest = liquid::hash(liquid::algorithm::blake3, root->entries.front().ref.bytes);
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

} // namespace
