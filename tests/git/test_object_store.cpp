#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "git/object_store.hpp"
#include "git/tree_order.hpp"
#include "liquid/git.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-git-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_ / "objects", code);
    }
    ~temporary_directory() {
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

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto run(std::string command) -> bool {
    return std::system(command.c_str()) == 0;
}

auto capture(const std::string& command) -> std::string {
    auto output = std::string{};
    const auto pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
        return output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        output += buffer.data();
    if (pclose(pipe) != 0)
        return {};
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
}

TEST(GitObjectStore, hashes_and_verifies_loose_objects) {
    EXPECT_EQ(liquid::git_detail::sha1_hex(liquid::git_detail::sha1_hash(bytes("abc"))),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    temporary_directory temporary;
    const auto id = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("contents"));
    ASSERT_TRUE(id) << id.error().message;
    liquid::git_detail::object_store store{temporary.path()};
    const auto object = store.read(*id);
    ASSERT_TRUE(object) << object.error().message;
    EXPECT_EQ(object->kind, liquid::git_detail::object_kind::blob);
    EXPECT_EQ(object->payload, bytes("contents"));
}

TEST(GitObjectStore, orders_tree_entries_as_git_does) {
    struct entry {
        std::string_view name;
        bool directory;
    };
    auto entries =
        std::vector<entry>{{"a", false}, {"a-b", false}, {"a", true}, {"a0", false}, {"ab", false}};
    std::ranges::sort(entries, [](const entry& left, const entry& right) {
        return liquid::git_detail::tree_entry_less(
            left.name, left.directory, right.name, right.directory);
    });
    EXPECT_EQ(entries[0].name, "a");
    EXPECT_FALSE(entries[0].directory);
    EXPECT_EQ(entries[1].name, "a-b");
    EXPECT_EQ(entries[2].name, "a");
    EXPECT_TRUE(entries[2].directory);
    EXPECT_EQ(entries[3].name, "a0");
    EXPECT_EQ(entries[4].name, "ab");
}

TEST(GitObjectStore, refuses_a_corrupt_loose_object) {
    temporary_directory temporary;
    const auto id = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("contents"));
    ASSERT_TRUE(id) << id.error().message;
    const auto name = liquid::git_detail::sha1_hex(*id);
    const auto path = temporary.path() / "objects" / name.substr(0, 2) / name.substr(2);
    auto output = std::fstream{path, std::ios::in | std::ios::out | std::ios::binary};
    ASSERT_TRUE(output);
    output.seekp(-1, std::ios::end);
    output.put('x');
    output.close();
    liquid::git_detail::object_store store{temporary.path()};
    const auto object = store.read(*id);
    ASSERT_FALSE(object);
    EXPECT_EQ(object.error().code, liquid::errc::corrupt);
}

TEST(GitObjectStore, reads_packed_objects_and_deltas) {
    if (!run("git --version >/dev/null 2>&1"))
        GTEST_SKIP() << "git is not installed";
    temporary_directory temporary;
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " init -q"));
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " config user.name Test"));
    ASSERT_TRUE(
        run("git -C " + temporary.path().string() + " config user.email test@example.test"));
    for (unsigned index = 0; index < 8; ++index) {
        auto output =
            std::ofstream{temporary.path() / "file.txt", std::ios::binary | std::ios::trunc};
        output << std::string(4096, 'a') << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + temporary.path().string() + " add file.txt"));
        ASSERT_TRUE(run("git -C " + temporary.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " gc --aggressive --prune=now"));
    const auto directory = liquid::git_detail::git_directory(temporary.path());
    ASSERT_TRUE(directory) << directory.error().message;
    liquid::git_detail::object_store store{*directory};
    const auto refs = store.refs();
    ASSERT_TRUE(refs) << refs.error().message;
    ASSERT_EQ(refs->size(), 1U);
    const auto commit = store.read_commit(refs->begin()->second);
    ASSERT_TRUE(commit) << commit.error().message;
    const auto tree = store.read_tree(commit->tree);
    ASSERT_TRUE(tree) << tree.error().message;
    ASSERT_EQ(tree->size(), 1U);
    const auto blob = store.read(tree->front().id);
    ASSERT_TRUE(blob) << blob.error().message;
    EXPECT_EQ(blob->kind, liquid::git_detail::object_kind::blob);
    EXPECT_EQ(blob->payload.size(), 4098U);
}

TEST(GitInteroperability, imports_exports_and_resumes_a_complete_history) {
    if (!run("git --version >/dev/null 2>&1"))
        GTEST_SKIP() << "git is not installed";
    temporary_directory source;
    temporary_directory destination;
    ASSERT_TRUE(run("git -C " + source.path().string() + " init -q"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " config user.name Test"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " config user.email test@example.test"));
    for (unsigned index = 0; index < 20; ++index) {
        auto output = std::ofstream{source.path() / "old.txt", std::ios::binary | std::ios::trunc};
        output << "value " << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + source.path().string() + " add old.txt"));
        ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " branch topic"));
    std::error_code rename_error;
    std::filesystem::rename(source.path() / "old.txt", source.path() / "new.txt", rename_error);
    ASSERT_FALSE(rename_error) << rename_error.message();
    ASSERT_TRUE(run("git -C " + source.path().string() + " add -A"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm rename"));
    for (unsigned index = 21; index < 48; ++index) {
        auto output = std::ofstream{source.path() / "new.txt", std::ios::binary | std::ios::trunc};
        output << "value " << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + source.path().string() + " add new.txt"));
        ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " checkout -q topic"));
    {
        auto output =
            std::ofstream{source.path() / "topic.txt", std::ios::binary | std::ios::trunc};
        output << "topic\n";
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " add topic.txt"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm topic"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " checkout -q -"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " merge --no-ff -qm merge topic"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " tag v1"));
    EXPECT_EQ(capture("git -C " + source.path().string() + " rev-list --all --count"), "50");

    auto created = liquid::runtime::block_on(liquid::repository::create(destination.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto imported = liquid::runtime::block_on(liquid::import_git(repository, source.path()));
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported->commits, 50U);
#if !defined(_WIN32)
    EXPECT_LT(imported->peak_anonymous_bytes, 1024ULL * 1024ULL * 1024ULL);
#endif
    const auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs) << refs.error().message;
    ASSERT_EQ(refs->size(), 3U);
    const auto main = std::ranges::find_if(
        *refs, [](const auto& value) { return value.name == "master" || value.name == "main"; });
    ASSERT_NE(main, refs->end());
    const auto history = liquid::runtime::block_on(repository.log({.from = main->id}));
    ASSERT_TRUE(history) << history.error().message;
    EXPECT_EQ(history->size(), 50U);
    const auto mapped = liquid::runtime::block_on(repository.metadata_keys("git.commit."));
    ASSERT_TRUE(mapped) << mapped.error().message;
    EXPECT_EQ(mapped->size(), 50U);
    const auto contents = liquid::runtime::block_on(repository.read("new.txt"));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ((std::string{reinterpret_cast<const char*>(contents->data()), contents->size()}),
              "value 47\n");

    const auto before_name = capture("git -C " + source.path().string() + " rev-parse HEAD~29");
    const auto after_name = capture("git -C " + source.path().string() + " rev-parse HEAD~28");
    const auto before_mapping =
        liquid::runtime::block_on(repository.metadata("git.commit." + before_name));
    const auto after_mapping =
        liquid::runtime::block_on(repository.metadata("git.commit." + after_name));
    ASSERT_TRUE(before_mapping) << before_mapping.error().message;
    ASSERT_TRUE(after_mapping) << after_mapping.error().message;
    ASSERT_TRUE(*before_mapping);
    ASSERT_TRUE(*after_mapping);
    const auto before_id = liquid::parse_digest(std::string{
        reinterpret_cast<const char*>((*before_mapping)->data()), (*before_mapping)->size()});
    const auto after_id = liquid::parse_digest(std::string{
        reinterpret_cast<const char*>((*after_mapping)->data()), (*after_mapping)->size()});
    ASSERT_TRUE(before_id) << before_id.error().message;
    ASSERT_TRUE(after_id) << after_id.error().message;
    const auto before = liquid::runtime::block_on(repository.at(liquid::change_id{*before_id}));
    const auto after = liquid::runtime::block_on(repository.at(liquid::change_id{*after_id}));
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_TRUE(after) << after.error().message;
    ASSERT_EQ(before->root.entries.size(), 1U);
    ASSERT_EQ(after->root.entries.size(), 1U);
    EXPECT_EQ(before->root.entries.front().name, "old.txt");
    EXPECT_EQ(after->root.entries.front().name, "new.txt");
    EXPECT_EQ(before->root.entries.front().node, after->root.entries.front().node);

    const auto resumed = liquid::runtime::block_on(liquid::import_git(repository, source.path()));
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_EQ(resumed->commits, 0U);
    EXPECT_EQ(resumed->skipped_commits, 50U);

    temporary_directory exported;
    const auto report =
        liquid::runtime::block_on(liquid::export_git(repository, exported.path(), main->name));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->commits, 50U);
    ASSERT_TRUE(run("git -C " + exported.path().string() + " fsck --no-dangling"));
    EXPECT_EQ(capture("git -C " + exported.path().string() + " rev-list --all --count"), "50");
    EXPECT_EQ(capture("git -C " + exported.path().string() + " show HEAD:new.txt"), "value 47");
}

} // namespace
