#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

#include "git/object_store.hpp"

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

} // namespace
