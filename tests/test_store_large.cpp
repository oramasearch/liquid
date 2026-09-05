#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "repository/testing.hpp"
#include "temporary_directory.hpp"

namespace {

class temporary_tree {
public:
    temporary_tree()
        : path_(liquid::test::fixture_path("liquid-store-large-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
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

auto marker() -> std::array<std::byte, 4096> {
    auto output = std::array<std::byte, 4096>{};
    auto state = std::uint64_t{22};
    for (auto& byte : output) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        byte = std::byte{static_cast<unsigned char>(state)};
    }
    return output;
}

void make_sparse_file(const std::filesystem::path& path,
                      std::uint64_t size,
                      std::span<const std::byte> contents) {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.close();
    std::filesystem::resize_file(path, size);
    auto file = std::fstream{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file);
    for (const auto offset :
         {std::uint64_t{0}, size / 2, size - static_cast<std::uint64_t>(contents.size())}) {
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
    }
    ASSERT_TRUE(file);
}

TEST(StoreLarge, Streams256MiBThroughRepositoryAndStore) {
    constexpr auto size = std::uint64_t{256} * 1024U * 1024U;
    temporary_tree temporary;
    const auto source = temporary.path() / "source.bin";
    const auto destination = temporary.path() / "destination.bin";
    const auto repository_path = temporary.path() / "repository";
    const auto expected = marker();
    make_sparse_file(source, size, expected);

    auto created = liquid::runtime::block_on(liquid::repository::create(repository_path));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto pending = repository.status()->pending_drips;
    auto written = liquid::runtime::block_on(repository.write_file("large.bin", source));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(repository.status()->pending_drips, pending + 1);

    const auto cache = repository_path / "LIQUID_VFS" / "chunks";
    auto hydrated = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        repository, "large.bin", cache, size / 2, expected.size()));
    ASSERT_TRUE(hydrated) << hydrated.error().message;
    EXPECT_EQ(hydrated->bytes, std::vector<std::byte>(expected.begin(), expected.end()));
    EXPECT_LT(hydrated->status.hydrated_bytes, size);
    repository = {};

    auto opened = liquid::runtime::block_on(liquid::store::open(repository_path / ".liquid-store"));
    ASSERT_TRUE(opened) << opened.error().message;
    auto store = std::move(*opened);
    auto id = liquid::runtime::block_on(store.put_file(source));
    ASSERT_TRUE(id) << id.error().message;
    for (const auto offset :
         {std::uint64_t{0}, size / 2, size - static_cast<std::uint64_t>(expected.size())}) {
        auto range = std::array<std::byte, 4096>{};
        auto read = liquid::runtime::block_on(store.read(*id, offset, range));
        ASSERT_TRUE(read) << read.error().message;
        ASSERT_EQ(*read, range.size());
        EXPECT_EQ(range, expected);
    }
    auto exported = liquid::runtime::block_on(store.get_to_file(*id, destination));
    ASSERT_TRUE(exported) << exported.error().message;
    auto exported_id = liquid::runtime::block_on(store.put_file(destination));
    ASSERT_TRUE(exported_id) << exported_id.error().message;
    EXPECT_EQ(exported_id->alg, id->alg);
    EXPECT_EQ(exported_id->bytes, id->bytes);
}

} // namespace
