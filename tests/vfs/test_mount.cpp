#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/xattr.h>
#endif

#include "../temporary_directory.hpp"
#include "drive/settings.hpp"
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

#if !defined(_WIN32)
class scoped_liquid_home {
public:
    explicit scoped_liquid_home(const std::filesystem::path& path) {
        if (const auto* configured = std::getenv("LIQUID_HOME")) {
            previous_ = configured;
        }
        set(path);
    }

    ~scoped_liquid_home() {
        if (previous_) {
            static_cast<void>(setenv("LIQUID_HOME", previous_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("LIQUID_HOME"));
        }
    }

    auto set(const std::filesystem::path& path) -> void {
        ASSERT_EQ(setenv("LIQUID_HOME", path.string().c_str(), 1), 0);
    }

private:
    std::optional<std::string> previous_;
};
#endif

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

class unavailable_source final : public liquid::remote_blob_source {
public:
    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest> blobs) override {
        co_return blobs;
    }

    liquid::runtime::task<std::uint64_t>
    read_range(std::string, liquid::digest, std::uint64_t, std::span<std::byte>) override {
        co_return std::unexpected(liquid::error{liquid::errc::unavailable, "test peer is offline"});
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest, std::vector<std::byte>) override {
        co_return std::unexpected(liquid::error{liquid::errc::unavailable, "test peer is offline"});
    }
};

TEST(VfsMount, hydrates_captures_and_reuses_cache) {
#if defined(_WIN32)
    GTEST_SKIP() << "Phase 1 provides materialise-only VFS support on Windows";
#else
    mount_tree repository_path;
    mount_tree mountpoint;
    mount_tree holder_identity;
    mount_tree writer_identity;
    auto identity_home = scoped_liquid_home{holder_identity.path()};
    auto remote_source = std::make_shared<unavailable_source>();
    auto options = liquid::repository_options{};
    options.storage.remote_sources.emplace("offline", remote_source);
    options.locks.rules.push_back({"locked-file", {"locked.txt"}});
    auto created = liquid::runtime::block_on(
        liquid::repository::create(repository_path.path(), std::move(options)));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    auto large = std::vector<std::byte>(64U * 1024U * 1024U);
    for (std::size_t index = 0; index < large.size(); ++index)
        large[index] = std::byte{static_cast<unsigned char>((index * 29U + 7U) & 0xffU)};
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("large.bin", large)));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("locked.txt", text_bytes("locked"))));
    const auto offline_contents = text_bytes("offline");
    auto offline_id = liquid::hash(liquid::algorithm::blake3, offline_contents);
    ASSERT_TRUE(offline_id);
    auto offline_blob = liquid::blob{.chunks = {*offline_id},
                                     .size = offline_contents.size(),
                                     .chunk_sizes = {offline_contents.size()},
                                     .content = *offline_id,
                                     .remote = liquid::remote_blob_location{"offline", "drive"}};
    ASSERT_TRUE(liquid::runtime::block_on(repository.write_remote("offline.bin", offline_blob)));
    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("locked.txt")));
    repository = {};
    identity_home.set(writer_identity.path());
    auto open_options = liquid::repository_options{};
    open_options.storage.remote_sources.emplace("offline", std::move(remote_source));
    auto opened = liquid::runtime::block_on(
        liquid::repository::open(repository_path.path(), std::move(open_options)));
    ASSERT_TRUE(opened) << opened.error().message;
    repository = std::move(*opened);

    auto mounted = liquid::runtime::block_on(liquid::vfs::mount(repository, mountpoint.path()));
    if (!mounted && mounted.error().code == liquid::errc::unsupported)
        GTEST_SKIP() << "FUSE unavailable: " << mounted.error().message;
    ASSERT_TRUE(mounted) << mounted.error().message << ": " << mounted.error().context;
    auto handle = std::move(*mounted);

#if defined(__linux__)
    const auto locked_path = mountpoint.path() / "locked.txt";
    constexpr auto pin_name = "user.liquid.pin";
    constexpr auto state_name = "user.liquid.state";
    ASSERT_EQ(::setxattr(locked_path.c_str(), pin_name, "1", 1, 0), 0) << std::strerror(errno);
    auto attribute = std::array<char, 32>{};
    const auto pin_size =
        ::getxattr(locked_path.c_str(), pin_name, attribute.data(), attribute.size());
    ASSERT_EQ(pin_size, 1) << std::strerror(errno);
    EXPECT_EQ((std::string_view{attribute.data(), static_cast<std::size_t>(pin_size)}), "1");
    const auto state_size =
        ::getxattr(locked_path.c_str(), state_name, attribute.data(), attribute.size());
    ASSERT_GT(state_size, 0) << std::strerror(errno);
    EXPECT_EQ((std::string_view{attribute.data(), static_cast<std::size_t>(state_size)}),
              "available-offline");
    const auto names_size = ::listxattr(locked_path.c_str(), nullptr, 0);
    ASSERT_GT(names_size, 0) << std::strerror(errno);
    auto names = std::string(static_cast<std::size_t>(names_size), '\0');
    ASSERT_EQ(::listxattr(locked_path.c_str(), names.data(), names.size()), names_size)
        << std::strerror(errno);
    EXPECT_NE(names.find(pin_name), std::string::npos);
    EXPECT_NE(names.find(state_name), std::string::npos);
    ASSERT_EQ(::setxattr(locked_path.c_str(), pin_name, "0", 1, 0), 0) << std::strerror(errno);

    const auto offline_descriptor = ::open((mountpoint.path() / "offline.bin").c_str(), O_RDONLY);
    ASSERT_GE(offline_descriptor, 0) << std::strerror(errno);
    auto offline_byte = std::byte{};
    errno = 0;
    EXPECT_EQ(::read(offline_descriptor, &offline_byte, 1), -1);
    EXPECT_EQ(errno, EAGAIN);
    ASSERT_EQ(::close(offline_descriptor), 0);
    auto drive_settings =
        liquid::runtime::block_on(liquid::drive_detail::load_settings(repository));
    ASSERT_TRUE(drive_settings);
    EXPECT_NE(drive_settings->offline_reason.find("peer unreachable while hydrating offline.bin"),
              std::string::npos);
#endif

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
