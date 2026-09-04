#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "repository/testing.hpp"

namespace {

class large_tree {
public:
    large_tree()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-large-40g-" + std::to_string(::getpid()))) {   // one directory per process: parallel runs never collide
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
    }

    ~large_tree() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

auto marker(std::uint64_t seed) -> std::array<std::byte, 4096> {
    auto output = std::array<std::byte, 4096>{};
    auto state = seed;
    for (auto& byte : output) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        byte = std::byte{static_cast<unsigned char>(state)};
    }
    return output;
}

auto random_fixture_requested() -> bool {
    const auto* value = std::getenv("LIQUID_40G_RANDOM");
    return value != nullptr && std::string_view{value} == "1";
}

auto next_random_word(std::uint64_t& state) -> std::uint64_t {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

void fill_random_bytes(std::span<std::byte> output, std::uint64_t& state) {
    auto offset = std::size_t{};
    while (offset + sizeof(std::uint64_t) <= output.size()) {
        const auto word = next_random_word(state);
        std::memcpy(output.data() + offset, &word, sizeof(word));
        offset += sizeof(word);
    }

    if (offset == output.size())
        return;

    const auto word = next_random_word(state);
    std::memcpy(output.data() + offset, &word, output.size() - offset);
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

void make_seeded_random_file(const std::filesystem::path& path, std::uint64_t size) {
    constexpr auto window_size = std::size_t{8} * 1024U * 1024U;
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    auto window = std::vector<std::byte>(window_size);
    auto state = std::uint64_t{116};
    auto remaining = size;
    while (remaining > 0) {
        const auto bytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, window.size()));
        fill_random_bytes(std::span{window}.first(bytes), state);
        output.write(reinterpret_cast<const char*>(window.data()),
                     static_cast<std::streamsize>(bytes));
        ASSERT_TRUE(output);
        remaining -= bytes;
    }
}

auto read_file_range(const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
    -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    EXPECT_TRUE(input);
    if (!input)
        return {};
    input.seekg(static_cast<std::streamoff>(offset));
    EXPECT_TRUE(input);
    if (!input)
        return {};
    auto bytes = std::vector<std::byte>(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    EXPECT_EQ(input.gcount(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return {};
    return bytes;
}

auto directory_size(const std::filesystem::path& root) -> std::uint64_t {
    auto total = std::uint64_t{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file())
            total += entry.file_size();
    }
    return total;
}

auto peak_rss_bytes() -> std::uint64_t {
#if defined(_WIN32)
    return 0;
#else
    auto usage = rusage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#endif
}

TEST(StoreLarge40G, StreamsFortyGiBAndFortyBytesWithBoundedMemory) {
#if defined(_WIN32)
    GTEST_SKIP() << "getrusage is required for the labelled memory assertion";
#else
    constexpr auto size = std::uint64_t{40} * 1024U * 1024U * 1024U;
    constexpr auto rss_limit = std::uint64_t{1} * 1024U * 1024U * 1024U;
    const auto started = std::chrono::steady_clock::now();
    large_tree temporary;
    const auto source = temporary.path() / "source-40g.bin";
    const auto destination = temporary.path() / "destination-40g.bin";
    const auto small_source = temporary.path() / "source-40b.bin";
    const auto repository_path = temporary.path() / "repository";
    const auto expected = marker(116);
    const auto random_fixture = random_fixture_requested();
    if (random_fixture)
        make_seeded_random_file(source, size);
    else
        make_sparse_file(source, size, expected);
    const auto small = std::array<std::byte, 40>{std::byte{'l'},
                                                 std::byte{'i'},
                                                 std::byte{'q'},
                                                 std::byte{'u'},
                                                 std::byte{'i'},
                                                 std::byte{'d'},
                                                 std::byte{'-'},
                                                 std::byte{'1'},
                                                 std::byte{'1'},
                                                 std::byte{'6'}};
    auto small_output = std::ofstream{small_source, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(small_output);
    small_output.write(reinterpret_cast<const char*>(small.data()), small.size());
    small_output.close();

    auto created = liquid::runtime::block_on(liquid::repository::create(repository_path));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto pending = repository.status()->pending_drips;
    const auto write_started = std::chrono::steady_clock::now();
    auto written = liquid::runtime::block_on(repository.write_file("large.bin", source));
    const auto write_finished = std::chrono::steady_clock::now();
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(repository.status()->pending_drips, pending + 1);
    auto small_written =
        liquid::runtime::block_on(repository.write_file("small.bin", small_source));
    ASSERT_TRUE(small_written) << small_written.error().message;
    EXPECT_EQ(repository.status()->pending_drips, pending + 2);
    auto small_read = liquid::runtime::block_on(repository.read("small.bin"));
    ASSERT_TRUE(small_read) << small_read.error().message;
    EXPECT_EQ(*small_read, std::vector<std::byte>(small.begin(), small.end()));

    const auto cache = repository_path / "LIQUID_VFS" / "chunks";
    const auto middle_expected = read_file_range(source, size / 2, expected.size());
    ASSERT_EQ(middle_expected.size(), expected.size());
    auto hydrated = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        repository, "large.bin", cache, size / 2, expected.size()));
    ASSERT_TRUE(hydrated) << hydrated.error().message;
    EXPECT_EQ(hydrated->bytes, middle_expected);
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
        const auto expected_range = read_file_range(source, offset, range.size());
        ASSERT_EQ(expected_range.size(), range.size());
        EXPECT_EQ(std::vector<std::byte>(range.begin(), range.end()), expected_range);
    }
    auto exported = liquid::runtime::block_on(store.get_to_file(*id, destination));
    ASSERT_TRUE(exported) << exported.error().message;
    auto exported_id = liquid::runtime::block_on(store.put_file(destination));
    ASSERT_TRUE(exported_id) << exported_id.error().message;
    EXPECT_EQ(exported_id->alg, id->alg);
    EXPECT_EQ(exported_id->bytes, id->bytes);

    const auto finished = std::chrono::steady_clock::now();
    const auto write_seconds =
        std::chrono::duration<double>(write_finished - write_started).count();
    const auto wall_seconds = std::chrono::duration<double>(finished - started).count();
    const auto throughput_mib = static_cast<double>(size) / (1024.0 * 1024.0) / write_seconds;
    const auto peak_rss = peak_rss_bytes();
    const auto store_bytes = directory_size(repository_path / ".liquid-store");
    std::cout << "LARGE_40G_METRICS"
              << " fixture=" << (random_fixture ? "seeded-random" : "sparse-zero-markers")
              << " write_seconds=" << write_seconds << " wall_seconds=" << wall_seconds
              << " write_mib_per_second=" << throughput_mib << " peak_rss_bytes=" << peak_rss
              << " store_bytes=" << store_bytes << '\n';
    ASSERT_GT(peak_rss, 0U);
    ASSERT_LT(peak_rss, rss_limit);
#endif
}

} // namespace
