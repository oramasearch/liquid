#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../temporary_directory.hpp"
#include "liquid/repository.hpp"
#include "liquid/vfs.hpp"
#include "repository/testing.hpp"

namespace {

class mount_tree {
public:
    mount_tree()
        : path_(liquid::test::fixture_path("liquid-mount-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~mount_tree() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto text_bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto node_for(const liquid::tree& root, std::string_view name) -> liquid::node_id {
    const auto found = std::ranges::find(root.entries, name, &liquid::tree_entry::name);
    return found == root.entries.end() ? liquid::node_id{} : found->node;
}

auto mode_for(const liquid::tree& root, std::string_view name) -> std::uint32_t {
    const auto found = std::ranges::find(root.entries, name, &liquid::tree_entry::name);
    return found == root.entries.end() ? 0U : found->mode;
}

TEST(VfsMount, hydrates_captures_and_reuses_cache) {
#if defined(_WIN32)
    GTEST_SKIP() << "Phase 1 provides materialise-only VFS support on Windows";
#else
    mount_tree repository_path;
    mount_tree mountpoint;
    auto created = liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    auto large = std::vector<std::byte>(64U * 1024U * 1024U);
    for (std::size_t index = 0; index < large.size(); ++index)
        large[index] = std::byte{static_cast<unsigned char>((index * 29U + 7U) & 0xffU)};
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("large.bin", large)));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("locked.txt", text_bytes("locked"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("locked.txt")));

    auto mounted = liquid::runtime::block_on(liquid::vfs::mount(repository, mountpoint.path()));
    if (!mounted && mounted.error().code == liquid::errc::unsupported)
        GTEST_SKIP() << "FUSE unavailable: " << mounted.error().message;
    ASSERT_TRUE(mounted) << mounted.error().message << ": " << mounted.error().context;
    auto handle = std::move(*mounted);

    constexpr std::uint64_t offset = 40ULL * 1024ULL * 1024ULL;
    auto range = std::vector<std::byte>(4096);
    const auto descriptor = ::open((mountpoint.path() / "large.bin").c_str(), O_RDONLY);
    ASSERT_GE(descriptor, 0) << std::strerror(errno);
    ASSERT_EQ(::pread(descriptor, range.data(), range.size(), static_cast<off_t>(offset)),
              static_cast<ssize_t>(range.size()));
    ASSERT_EQ(::close(descriptor), 0);
    EXPECT_TRUE(std::equal(range.begin(), range.end(), large.begin() + offset));
    auto partial = handle.status();
    ASSERT_TRUE(partial);
    EXPECT_LT(partial->hydrated_bytes, large.size());
    EXPECT_EQ(partial->hydrated_files, 1U);
    EXPECT_GT(partial->misses_served, 0U);

    auto whole = std::vector<std::byte>(large.size());
    auto input = std::ifstream{mountpoint.path() / "large.bin", std::ios::binary};
    ASSERT_TRUE(input);
    input.read(reinterpret_cast<char*>(whole.data()), static_cast<std::streamsize>(whole.size()));
    ASSERT_TRUE(input);
    EXPECT_EQ(whole, large);

    const auto pending_before_write = repository.status()->pending_drips;
    {
        auto output = std::ofstream{mountpoint.path() / "written.txt", std::ios::binary};
        ASSERT_TRUE(output);
        output << "captured";
    }
    auto written = liquid::runtime::block_on(repository.read("written.txt"));
    ASSERT_TRUE(written);
    EXPECT_EQ(written, text_bytes("captured"));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    const auto written_node = node_for(*root, "written.txt");
    EXPECT_EQ(liquid::repository_detail::testing::last_operation_node(repository), written_node);
    EXPECT_GT(repository.status()->pending_drips, pending_before_write);

    const auto executable_path = mountpoint.path() / "executable";
    const auto executable = ::open(executable_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    ASSERT_GE(executable, 0) << std::strerror(errno);
    ASSERT_EQ(::write(executable, "first", 5), 5);
    ASSERT_EQ(::close(executable), 0);
    root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    EXPECT_EQ(mode_for(*root, "executable"), 0100755U);

    ASSERT_EQ(::chmod(executable_path.c_str(), 0700), 0) << std::strerror(errno);
    const auto rewritten = ::open(executable_path.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(rewritten, 0) << std::strerror(errno);
    ASSERT_EQ(::write(rewritten, "second", 6), 6);
    ASSERT_EQ(::close(rewritten), 0);
    root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    EXPECT_EQ(mode_for(*root, "executable"), 0100700U);

    const auto pending_before_move = repository.status()->pending_drips;
    std::filesystem::rename(mountpoint.path() / "written.txt", mountpoint.path() / "renamed.txt");
    EXPECT_EQ(repository.status()->pending_drips, pending_before_move + 1);
    auto renamed = liquid::runtime::block_on(repository.read("renamed.txt"));
    EXPECT_EQ(renamed, text_bytes("captured"));

    errno = 0;
    const auto locked = ::open((mountpoint.path() / "locked.txt").c_str(), O_WRONLY);
    ASSERT_GE(locked, 0) << std::strerror(errno);
    errno = 0;
    EXPECT_EQ(::write(locked, "x", 1), -1);
    EXPECT_EQ(errno, EACCES);
    ASSERT_EQ(::close(locked), 0);

    std::filesystem::create_directory(mountpoint.path() / "empty");
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint.path() / "empty"));
    std::filesystem::remove(mountpoint.path() / "empty");
    std::filesystem::remove(mountpoint.path() / "renamed.txt");
    std::filesystem::remove(executable_path);

    auto active = handle.status();
    ASSERT_TRUE(active);
    EXPECT_GT(active->writes_captured, 0U);
    EXPECT_GT(active->request_queue_latency.count(), 0);
    ASSERT_TRUE(liquid::runtime::block_on(handle.unmount()));

    auto remounted = liquid::runtime::block_on(liquid::vfs::mount(repository, mountpoint.path()));
    ASSERT_TRUE(remounted) << remounted.error().message;
    auto second = std::move(*remounted);
    range.assign(range.size(), std::byte{});
    const auto cached_descriptor = ::open((mountpoint.path() / "large.bin").c_str(), O_RDONLY);
    ASSERT_GE(cached_descriptor, 0) << std::strerror(errno);
    ASSERT_EQ(::pread(cached_descriptor, range.data(), range.size(), static_cast<off_t>(offset)),
              static_cast<ssize_t>(range.size()));
    ASSERT_EQ(::close(cached_descriptor), 0);
    auto cached = second.status();
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached->misses_served, 0U);
    ASSERT_TRUE(liquid::runtime::block_on(second.unmount()));
#endif
}

} // namespace
