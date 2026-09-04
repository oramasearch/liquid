#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "liquid/store.hpp"
#include "store/chunker.hpp"
#include "store/pack.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory() {
        static std::uint64_t sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("liquid-store-test-" + std::to_string(++sequence) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~temporary_directory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

liquid::store_options test_options(liquid::algorithm algorithm = liquid::algorithm::blake3) {
    liquid::store_options options;
    options.hash_algorithm = algorithm;
    options.index_cache_size = 8 * 1024 * 1024;
    return options;
}

std::vector<std::byte> make_bytes(std::size_t size, std::uint64_t seed = 1) {
    std::mt19937_64 random(seed);
    std::vector<std::byte> output(size);
    for (auto& byte : output) byte = std::byte{static_cast<unsigned char>(random())};
    return output;
}

std::uint64_t pack_bytes(const std::filesystem::path& root) {
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "packs")) {
        if (entry.path().extension() == ".pack") total += entry.file_size();
    }
    return total;
}

std::set<std::string> chunk_ids(std::span<const std::byte> input) {
    std::set<std::string> output;
    for (const auto& range : liquid::store_detail::chunk(input)) {
        auto id = liquid::hash(liquid::algorithm::blake3, input.subspan(range.offset, range.length));
        EXPECT_TRUE(id.has_value());
        if (id) output.insert(liquid::to_string(*id));
    }
    return output;
}

}  // namespace

TEST(Chunker, DeterministicSizesAndCoverage) {
    const auto input = make_bytes(4 * 1024 * 1024);
    const auto first = liquid::store_detail::chunk(input);
    const auto second = liquid::store_detail::chunk(input);
    ASSERT_EQ(first.size(), second.size());
    std::size_t covered = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].offset, second[index].offset);
        EXPECT_EQ(first[index].length, second[index].length);
        EXPECT_EQ(first[index].offset, covered);
        EXPECT_GE(first[index].length, liquid::store_detail::chunk_min_size);
        EXPECT_LE(first[index].length, liquid::store_detail::chunk_max_size);
        covered += first[index].length;
    }
    EXPECT_EQ(covered, input.size());
}

TEST(Chunker, FrontInsertionChangesOnlyNearbyChunks) {
    const auto original = make_bytes(4 * 1024 * 1024, 91);
    auto inserted = make_bytes(31, 92);
    inserted.insert(inserted.end(), original.begin(), original.end());
    const auto before = chunk_ids(original);
    const auto after = chunk_ids(inserted);
    std::size_t shared = 0;
    for (const auto& id : before) shared += after.contains(id) ? 1 : 0;
    EXPECT_LE(before.size() - shared, 4U);
    EXPECT_LE(after.size() - shared, 5U);
}

TEST(Store, FortyBytesRoundTripAndLayout) {
    temporary_directory directory;
    auto opened = liquid::store::open(directory.path(), test_options());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    const auto input = make_bytes(40);
    auto id = opened->put(input);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    auto present = opened->contains(*id);
    ASSERT_TRUE(present.has_value());
    EXPECT_TRUE(*present);
    auto output = opened->get(*id);
    ASSERT_TRUE(output.has_value()) << output.error().message;
    EXPECT_EQ(*output, input);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path() / "LIQUID_STORE"));
    EXPECT_TRUE(std::filesystem::is_directory(directory.path() / "packs"));
    EXPECT_TRUE(std::filesystem::is_directory(directory.path() / "index"));
    EXPECT_TRUE(std::filesystem::is_directory(directory.path() / "tmp"));
}

TEST(Store, ReadsOnlyTheRangeAcrossChunkBoundaries) {
    temporary_directory directory;
    auto input = make_bytes(2 * 1024 * 1024, 12);
    const auto chunks = liquid::store_detail::chunk(input);
    ASSERT_GT(chunks.size(), 2U);
    const auto boundary = chunks[1].offset;
    auto opened = liquid::store::open(directory.path(), test_options());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    auto id = opened->put(input);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    std::array<std::byte, 257> output{};
    const auto offset = boundary - 113;
    auto count = opened->read(*id, offset, output);
    ASSERT_TRUE(count.has_value()) << count.error().message;
    ASSERT_EQ(*count, output.size());
    EXPECT_TRUE(std::equal(output.begin(), output.end(), input.begin() + offset));
}

TEST(Store, DuplicateInputAddsNoPackBytes) {
    temporary_directory directory;
    const auto input = make_bytes(10 * 1024 * 1024, 20);
    auto opened = liquid::store::open(directory.path(), test_options());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    auto first = opened->put(input);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto after_first = pack_bytes(directory.path());
    auto second = opened->put(input);
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
    EXPECT_EQ(pack_bytes(directory.path()), after_first);
}

TEST(Store, DetectsPackCorruptionWithDigestContext) {
    temporary_directory directory;
    const auto input = make_bytes(2 * 1024 * 1024, 30);
    auto opened = liquid::store::open(directory.path(), test_options());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    auto id = opened->put(input);
    ASSERT_TRUE(id.has_value()) << id.error().message;

    const auto pack = directory.path() / "packs" / "0.pack";
    std::fstream file(pack, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.good());
    file.seekg(static_cast<std::streamoff>(liquid::store_detail::pack_record_header_size));
    char value = 0;
    file.read(&value, 1);
    value ^= 0x5a;
    file.seekp(static_cast<std::streamoff>(liquid::store_detail::pack_record_header_size));
    file.write(&value, 1);
    file.flush();

    auto output = opened->get(*id);
    ASSERT_FALSE(output.has_value());
    EXPECT_EQ(output.error().code, liquid::errc::corrupt);
    EXPECT_FALSE(output.error().context.empty());
}

TEST(Store, ReopensAndReadsBothDigestAlgorithms) {
    temporary_directory directory;
    const auto input = make_bytes(1024 * 1024, 40);
    liquid::digest id{};
    {
        auto opened = liquid::store::open(
            directory.path(), test_options(liquid::algorithm::sha256));
        ASSERT_TRUE(opened.has_value()) << opened.error().message;
        auto stored = opened->put(input);
        ASSERT_TRUE(stored.has_value()) << stored.error().message;
        id = *stored;
    }
    auto reopened = liquid::store::open(directory.path(), test_options());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message;
    auto output = reopened->get(id);
    ASSERT_TRUE(output.has_value()) << output.error().message;
    EXPECT_EQ(*output, input);
}

TEST(Store, RefusesUnknownFormatVersion) {
    temporary_directory directory;
    {
        std::ofstream marker(directory.path() / "LIQUID_STORE", std::ios::binary);
        marker << "LIQUID_STORE\nversion=999\n";
    }
    auto opened = liquid::store::open(directory.path(), test_options());
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code, liquid::errc::unsupported);
}

TEST(Store, LeasesAndCollectionRemainUnsupported) {
    liquid::store unopened;
    const liquid::digest id{liquid::algorithm::blake3, {}};
    EXPECT_EQ(unopened.put({}).error().code, liquid::errc::invalid_argument);
    EXPECT_EQ(unopened.lease_root(id, std::chrono::seconds{1}).error().code, liquid::errc::unsupported);
    EXPECT_EQ(unopened.collect().error().code, liquid::errc::unsupported);
}
