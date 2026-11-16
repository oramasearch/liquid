// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../temporary_directory.hpp"
#include "liquid/repository.hpp"
#include "liquid/vfs.hpp"
#include "repository/testing.hpp"

namespace {

class temporary_tree {
public:
    temporary_tree()
        : path_(liquid::test::fixture_path("liquid-vfs-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_tree() {
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

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(VfsMaterialise, reproduces_repository_and_selected_prefix) {
    temporary_tree repository_path;
    temporary_tree checkout_path;
    temporary_tree prefix_path;
    auto opened = liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("README.txt", bytes("root\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("docs/first.txt", bytes("first\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("docs/nested/second.bin", bytes(std::string_view{"second\0", 7}))));

    auto materialised =
        liquid::runtime::block_on(liquid::vfs::materialise(repository, checkout_path.path()));
    ASSERT_TRUE(materialised) << materialised.error().message;
    EXPECT_EQ(read_text(checkout_path.path() / "README.txt"), "root\n");
    EXPECT_EQ(read_text(checkout_path.path() / "docs/first.txt"), "first\n");
    EXPECT_EQ(read_text(checkout_path.path() / "docs/nested/second.bin"),
              std::string("second\0", 7));
    EXPECT_EQ(std::distance(std::filesystem::recursive_directory_iterator{checkout_path.path()},
                            std::filesystem::recursive_directory_iterator{}),
              5);
#if !defined(_WIN32)
    const auto file_permissions =
        std::filesystem::status(checkout_path.path() / "README.txt").permissions() &
        std::filesystem::perms::all;
    const auto directory_permissions =
        std::filesystem::status(checkout_path.path() / "docs").permissions() &
        std::filesystem::perms::all;
    EXPECT_EQ(file_permissions,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                  std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    EXPECT_EQ(directory_permissions,
              std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                  std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                  std::filesystem::perms::others_exec);
#endif

    auto selected =
        liquid::runtime::block_on(liquid::vfs::materialise(repository, prefix_path.path(), "docs"));
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_FALSE(std::filesystem::exists(prefix_path.path() / "README.txt"));
    EXPECT_EQ(read_text(prefix_path.path() / "first.txt"), "first\n");
    EXPECT_EQ(read_text(prefix_path.path() / "nested/second.bin"), std::string("second\0", 7));
}

TEST(VfsHydration, fetches_only_chunks_covering_the_requested_range) {
    temporary_tree repository_path;
    auto opened = liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    auto contents = std::vector<std::byte>(4U * 1024U * 1024U);
    for (std::size_t index = 0; index < contents.size(); ++index)
        contents[index] = std::byte{static_cast<unsigned char>((index * 17U) & 0xffU)};
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("large.bin", contents)));
    const auto cache_path = repository_path.path() / "LIQUID_VFS" / "chunks";
    constexpr std::uint64_t offset = 2U * 1024U * 1024U;
    auto range = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        repository, "large.bin", cache_path, offset, 4096));
    ASSERT_TRUE(range) << range.error().message;
    ASSERT_EQ(range->bytes.size(), 4096U);
    EXPECT_TRUE(std::equal(range->bytes.begin(), range->bytes.end(), contents.begin() + offset));
    const auto status = range->status;
    EXPECT_LT(status.hydrated_bytes, contents.size());
    EXPECT_GT(status.hydrated_bytes, 0U);
    EXPECT_EQ(status.hydrated_files, 1U);
    EXPECT_GE(status.misses_served, 1U);

    auto cached = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        repository, "large.bin", cache_path, offset, 4096));
    ASSERT_TRUE(cached) << cached.error().message;
    EXPECT_EQ(cached->status.misses_served, 0U);

    for (const auto& entry : std::filesystem::directory_iterator{cache_path}) {
        const auto size = entry.file_size();
        auto corrupt = std::ofstream{entry.path(), std::ios::binary | std::ios::trunc};
        auto zeroes = std::vector<char>(static_cast<std::size_t>(size));
        corrupt.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    }
    auto refused = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        repository, "large.bin", cache_path, offset, 4096));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
}

} // namespace
