#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "store/chunker.hpp"
#include "temporary_directory.hpp"

namespace {

class temporary_store {
public:
    temporary_store()
        : path_(liquid::test::fixture_path("liquid-store-test-" + std::to_string(++counter_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_store() {
        std::filesystem::remove_all(path_);
    }
    const std::filesystem::path& path() const {
        return path_;
    }

private:
    inline static unsigned counter_{};
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::size_t size, std::uint64_t seed = 9) {
    std::vector<std::byte> output(size);
    std::mt19937_64 random(seed);
    for (auto& byte : output)
        byte = std::byte{static_cast<unsigned char>(random())};
    return output;
}

liquid::store open_store(const std::filesystem::path& path, liquid::store_options options = {}) {
    auto opened = liquid::runtime::block_on(liquid::store::open(path, std::move(options)));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

std::shared_ptr<liquid::keyring> keys_for(std::string id, std::string passphrase) {
    auto keys = std::make_shared<liquid::keyring>();
    EXPECT_TRUE(keys->add(id, std::make_shared<liquid::passphrase_provider>(std::move(passphrase)))
                    .has_value());
    EXPECT_TRUE(keys->set_default_key_id(id).has_value());
    return keys;
}

liquid::store_options encrypted_options(const std::shared_ptr<liquid::keyring>& keys,
                                        std::string write_key = {}) {
    liquid::store_options options;
    options.keys = keys;
    options.write_key = std::move(write_key);
    return options;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> contents) {
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output);
}

auto read_bytes(const std::filesystem::path& path) -> std::vector<char> {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto store_files(const std::filesystem::path& root)
    -> std::map<std::filesystem::path, std::vector<char>> {
    auto files = std::map<std::filesystem::path, std::vector<char>>{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.emplace(std::filesystem::relative(entry.path(), root), read_bytes(entry.path()));
        }
    }
    return files;
}

auto pack_bytes(const std::filesystem::path& root) -> std::uint64_t {
    auto total = std::uint64_t{};
    for (const auto& entry : std::filesystem::directory_iterator(root / "packs")) {
        if (entry.is_regular_file() && entry.path().extension() == ".pack") {
            total += entry.file_size();
        }
    }
    return total;
}

struct write_observation {
    liquid::digest content{};
    std::vector<std::byte> bytes;
    std::vector<liquid::digest> chunks;
    std::vector<std::uint64_t> chunk_sizes;
    bool resumed_on_executor{};
    bool pack_unchanged_while_waiting{};
};

class recording_write_through final : public liquid::blob_write_through {
public:
    explicit recording_write_through(std::filesystem::path root) : root_(std::move(root)) {}

    void fail(liquid::digest content) {
        failing_ = liquid::to_string(content);
    }

    auto observations() const -> const std::vector<write_observation>& {
        return observations_;
    }

    liquid::runtime::task<void> put(liquid::digest content,
                                    std::span<const std::byte> contents,
                                    std::span<const liquid::digest> chunks,
                                    std::span<const std::uint64_t> chunk_sizes) override {
        const auto before_executor = co_await boost::capy::this_coro::executor;
        const auto before_pack = pack_bytes(root_);
        auto [wait_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{1});
        if (wait_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, wait_error.message(), "write-through test"});
        }
        const auto after_executor = co_await boost::capy::this_coro::executor;
        observations_.push_back(write_observation{content,
                                                  {contents.begin(), contents.end()},
                                                  {chunks.begin(), chunks.end()},
                                                  {chunk_sizes.begin(), chunk_sizes.end()},
                                                  before_executor == after_executor,
                                                  before_pack == pack_bytes(root_)});
        if (failing_ && *failing_ == liquid::to_string(content)) {
            co_return std::unexpected(liquid::error{liquid::errc::unavailable,
                                                    "write-through refused content",
                                                    liquid::to_string(content)});
        }
        co_return liquid::result<void>{};
    }

private:
    std::filesystem::path root_;
    std::optional<std::string> failing_;
    std::vector<write_observation> observations_;
};

class memory_blob_service final : public liquid::blob_write_through,
                                  public liquid::remote_blob_source {
public:
    liquid::runtime::task<void> put(liquid::digest content,
                                    std::span<const std::byte> contents,
                                    std::span<const liquid::digest> chunks,
                                    std::span<const std::uint64_t> chunk_sizes) override {
        content_ = content;
        bytes_.assign(contents.begin(), contents.end());
        chunks_.assign(chunks.begin(), chunks.end());
        chunk_sizes_.assign(chunk_sizes.begin(), chunk_sizes.end());
        co_return liquid::result<void>{};
    }

    auto registered_blob() const -> liquid::blob {
        return liquid::blob{chunks_,
                            bytes_.size(),
                            chunk_sizes_,
                            content_,
                            liquid::remote_blob_location{"memory", "durable"}};
    }

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string instance, std::vector<liquid::digest> blobs) override {
        if (instance != "durable") {
            co_return std::unexpected(liquid::error{
                liquid::errc::invalid_argument, "unknown instance", std::move(instance)});
        }
        auto missing = std::vector<liquid::digest>{};
        for (const auto& blob : blobs) {
            if (liquid::to_string(blob) != liquid::to_string(content_)) {
                missing.push_back(blob);
            }
        }
        co_return missing;
    }

    liquid::runtime::task<std::uint64_t> read_range(std::string instance,
                                                    liquid::digest content,
                                                    std::uint64_t offset,
                                                    std::span<std::byte> output) override {
        if (instance != "durable" || liquid::to_string(content) != liquid::to_string(content_)) {
            co_return std::unexpected(liquid::error{
                liquid::errc::not_found, "remote content is unavailable", std::move(instance)});
        }
        if (offset >= bytes_.size()) {
            co_return std::uint64_t{};
        }
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(output.size(), bytes_.size() - offset));
        std::copy_n(bytes_.begin() + static_cast<std::size_t>(offset), count, output.begin());
        co_return count;
    }

    liquid::runtime::task<void>
    write(std::string instance, liquid::digest content, std::vector<std::byte> contents) override {
        if (instance != "durable") {
            co_return std::unexpected(liquid::error{
                liquid::errc::invalid_argument, "unknown instance", std::move(instance)});
        }
        auto actual = liquid::hash(content.alg, contents);
        if (!actual || liquid::to_string(*actual) != liquid::to_string(content)) {
            co_return std::unexpected(liquid::error{
                liquid::errc::corrupt, "remote write digest mismatch", liquid::to_string(content)});
        }
        content_ = content;
        bytes_ = std::move(contents);
        co_return liquid::result<void>{};
    }

private:
    liquid::digest content_{};
    std::vector<std::byte> bytes_;
    std::vector<liquid::digest> chunks_;
    std::vector<std::uint64_t> chunk_sizes_;
};

struct concurrent_put_results {
    liquid::result<liquid::digest> failing;
    liquid::result<liquid::digest> successful;
};

liquid::runtime::task<concurrent_put_results> concurrent_puts(liquid::store* store,
                                                              std::vector<std::byte> failing,
                                                              std::vector<std::byte> successful) {
    auto [wait_error, first, second] = co_await liquid::runtime::when_all(
        liquid::runtime::as_io(store->put(std::move(failing))),
        liquid::runtime::as_io(store->put(std::move(successful))));
    if (wait_error) {
        co_return std::unexpected(liquid::error{
            liquid::errc::internal, wait_error.message(), "concurrent store put test"});
    }
    co_return concurrent_put_results{std::move(first), std::move(second)};
}

TEST(Store, WriteThroughFailureLeavesStoreUnchanged) {
    temporary_store temporary;
    const auto source = bytes(512U * 1024U, 201);
    const auto expected = liquid::hash(liquid::algorithm::blake3, source);
    ASSERT_TRUE(expected) << expected.error().message;
    auto hook = std::make_shared<recording_write_through>(temporary.path());
    hook->fail(*expected);
    auto options = liquid::store_options{};
    options.write_through = hook;
    auto store = open_store(temporary.path(), std::move(options));
    const auto before = store_files(temporary.path());

    const auto stored = liquid::runtime::block_on(store.put(source));

    ASSERT_FALSE(stored);
    EXPECT_EQ(stored.error().code, liquid::errc::unavailable);
    EXPECT_EQ(store_files(temporary.path()), before);
    const auto availability = liquid::runtime::block_on(store.has(*expected));
    ASSERT_TRUE(availability) << availability.error().message;
    EXPECT_EQ(*availability, liquid::blob_availability::absent);
    ASSERT_EQ(hook->observations().size(), 1U);
    EXPECT_TRUE(hook->observations().front().resumed_on_executor);
    EXPECT_TRUE(hook->observations().front().pack_unchanged_while_waiting);
}

TEST(Store, WriteThroughReceivesWholeMappedFileAndChunkTable) {
    temporary_store temporary;
    temporary_store source_root;
    const auto source = bytes(8U * 1024U * 1024U + 117U, 202);
    const auto source_path = source_root.path() / "source.bin";
    write_bytes(source_path, source);
    auto hook = std::make_shared<recording_write_through>(temporary.path());
    auto options = liquid::store_options{};
    options.write_through = hook;
    auto store = open_store(temporary.path(), std::move(options));

    const auto stored = liquid::runtime::block_on(store.put_file(source_path));

    ASSERT_TRUE(stored) << stored.error().message;
    const auto stored_again = liquid::runtime::block_on(store.put_file(source_path));
    ASSERT_TRUE(stored_again) << stored_again.error().message;
    EXPECT_EQ(liquid::to_string(*stored_again), liquid::to_string(*stored));
    ASSERT_EQ(hook->observations().size(), 1U);
    const auto& observed = hook->observations().front();
    EXPECT_EQ(liquid::to_string(observed.content), liquid::to_string(*stored));
    EXPECT_EQ(observed.bytes, source);
    EXPECT_EQ(observed.chunks.size(), observed.chunk_sizes.size());
    EXPECT_EQ(
        std::accumulate(observed.chunk_sizes.begin(), observed.chunk_sizes.end(), std::uint64_t{}),
        source.size());
    auto offset = std::size_t{};
    for (auto index = std::size_t{}; index < observed.chunks.size(); ++index) {
        const auto size = static_cast<std::size_t>(observed.chunk_sizes[index]);
        const auto actual = liquid::hash(observed.chunks[index].alg,
                                         std::span<const std::byte>(source).subspan(offset, size));
        ASSERT_TRUE(actual) << actual.error().message;
        EXPECT_EQ(liquid::to_string(*actual), liquid::to_string(observed.chunks[index]));
        offset += size;
    }
    EXPECT_EQ(offset, source.size());
    EXPECT_TRUE(observed.resumed_on_executor);
    EXPECT_TRUE(observed.pack_unchanged_while_waiting);
}

TEST(Store, ConcurrentWriteThroughFailureDoesNotAffectSuccessfulPut) {
    temporary_store temporary;
    const auto failing_source = bytes(768U * 1024U, 203);
    const auto successful_source = bytes(768U * 1024U, 204);
    const auto failing_id = liquid::hash(liquid::algorithm::blake3, failing_source);
    ASSERT_TRUE(failing_id) << failing_id.error().message;
    auto hook = std::make_shared<recording_write_through>(temporary.path());
    hook->fail(*failing_id);
    auto options = liquid::store_options{};
    options.write_through = hook;
    auto store = open_store(temporary.path(), std::move(options));

    const auto results =
        liquid::runtime::block_on(concurrent_puts(&store, failing_source, successful_source));

    ASSERT_TRUE(results) << results.error().message;
    ASSERT_FALSE(results->failing);
    EXPECT_EQ(results->failing.error().code, liquid::errc::unavailable);
    ASSERT_TRUE(results->successful) << results->successful.error().message;
    const auto failed_availability = liquid::runtime::block_on(store.has(*failing_id));
    ASSERT_TRUE(failed_availability) << failed_availability.error().message;
    EXPECT_EQ(*failed_availability, liquid::blob_availability::absent);
    const auto stored = liquid::runtime::block_on(store.get(*results->successful));
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(*stored, successful_source);
    ASSERT_EQ(hook->observations().size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(hook->observations(), [](const auto& observation) {
        return observation.resumed_on_executor && observation.pack_unchanged_while_waiting;
    }));
}

TEST(Store, EvictsRemoteRegisteredContentAndHydratesItAgain) {
    temporary_store temporary;
    const auto source = bytes(768U * 1024U, 205);
    auto service = std::make_shared<memory_blob_service>();
    auto options = liquid::store_options{};
    options.write_through = service;
    options.remote_sources.emplace("memory", service);
    auto store = open_store(temporary.path(), options);
    const auto stored = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(stored) << stored.error().message;
    const auto remote = service->registered_blob();
    ASSERT_GE(remote.chunks.size(), 3U);
    const auto registered = liquid::runtime::block_on(store.register_remote(remote));
    ASSERT_TRUE(registered) << registered.error().message;
    const auto bytes_before = liquid::runtime::block_on(store.local_bytes());
    ASSERT_TRUE(bytes_before) << bytes_before.error().message;
    ASSERT_GE(*bytes_before, source.size());

    const auto evicted = liquid::runtime::block_on(store.evict(*stored));
    ASSERT_TRUE(evicted) << evicted.error().message;
    const auto bytes_after = liquid::runtime::block_on(store.local_bytes());
    ASSERT_TRUE(bytes_after) << bytes_after.error().message;
    EXPECT_EQ(*bytes_after, 0U);
    const auto remote_only = liquid::runtime::block_on(store.has(*stored));
    ASSERT_TRUE(remote_only) << remote_only.error().message;
    EXPECT_EQ(*remote_only, liquid::blob_availability::remote);
    const auto collected = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_GT(collected->packs_deleted, 0U);

    store = {};
    auto hydrated_chunks = std::uint64_t{};
    auto hydrated_bytes = std::uint64_t{};
    auto remote_accesses = std::uint64_t{};
    options.remote_sources.clear();
    options.on_hydration = [&](liquid::digest content,
                               liquid::digest,
                               std::uint64_t count,
                               bool) -> liquid::runtime::task<void> {
        EXPECT_EQ(liquid::to_string(content), liquid::to_string(*stored));
        ++hydrated_chunks;
        hydrated_bytes += count;
        co_return liquid::result<void>{};
    };
    options.on_remote_access = [&](liquid::digest content) {
        EXPECT_EQ(liquid::to_string(content), liquid::to_string(*stored));
        ++remote_accesses;
    };
    store = open_store(temporary.path(), options);
    const auto added_source =
        liquid::runtime::block_on(store.register_remote_source("memory", service));
    ASSERT_TRUE(added_source) << added_source.error().message;
    const auto after_reopen = liquid::runtime::block_on(store.has(*stored));
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(*after_reopen, liquid::blob_availability::remote);
    const auto offset = remote.chunk_sizes.front() - 1U;
    auto range = std::vector<std::byte>(static_cast<std::size_t>(remote.chunk_sizes[1] + 2U));
    const auto read = liquid::runtime::block_on(store.read(*stored, offset, range));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, range.size());
    EXPECT_EQ(
        range,
        std::vector<std::byte>(source.begin() + static_cast<std::size_t>(offset),
                               source.begin() + static_cast<std::size_t>(offset) + range.size()));
    const auto range_chunks = hydrated_chunks;
    const auto range_bytes = hydrated_bytes;
    EXPECT_GE(range_chunks, 3U);
    EXPECT_EQ(remote_accesses, 1U);

    const auto local_read = liquid::runtime::block_on(store.read(*stored, offset, range));
    ASSERT_TRUE(local_read) << local_read.error().message;
    EXPECT_EQ(remote_accesses, 2U);

    const auto evicted_again = liquid::runtime::block_on(store.evict(*stored));
    ASSERT_TRUE(evicted_again) << evicted_again.error().message;
    const auto hydrated = liquid::runtime::block_on(store.get(*stored));
    ASSERT_TRUE(hydrated) << hydrated.error().message;
    EXPECT_EQ(*hydrated, source);
    EXPECT_EQ(hydrated_chunks, range_chunks + remote.chunks.size());
    EXPECT_EQ(hydrated_bytes, range_bytes + source.size());
    EXPECT_EQ(remote_accesses, 3U);
}

TEST(Store, RefusesToEvictContentThatIsNotRegisteredRemote) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(256U * 1024U, 206);
    const auto stored = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(stored) << stored.error().message;

    const auto evicted = liquid::runtime::block_on(store.evict(*stored));

    ASSERT_FALSE(evicted);
    EXPECT_EQ(evicted.error().code, liquid::errc::not_remote);
    const auto retained = liquid::runtime::block_on(store.get(*stored));
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(*retained, source);
}

TEST(Store, ChunkingProducesStableObjectIdentity) {
    const auto source = bytes(4U * 1024U * 1024U, 4);
    temporary_store first_path;
    temporary_store second_path;
    auto first_store = open_store(first_path.path());
    auto second_store = open_store(second_path.path());
    const auto first = liquid::runtime::block_on(first_store.put(source));
    const auto second = liquid::runtime::block_on(second_store.put(source));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
}

TEST(Store, StreamingChunkerMatchesInMemoryBoundaries) {
    const auto source = bytes(3U * 1024U * 1024U + 317U, 24);
    const auto expected = liquid::store_detail::fastcdc(source);
    auto streaming = liquid::store_detail::chunker{};
    auto actual = std::vector<std::vector<std::byte>>{};
    for (std::size_t offset = 0; offset < source.size();) {
        const auto size = std::min<std::size_t>(7919, source.size() - offset);
        auto chunks = streaming.feed(std::span<const std::byte>(source).subspan(offset, size));
        actual.insert(actual.end(),
                      std::make_move_iterator(chunks.begin()),
                      std::make_move_iterator(chunks.end()));
        offset += size;
    }
    auto final = streaming.finish();
    actual.insert(
        actual.end(), std::make_move_iterator(final.begin()), std::make_move_iterator(final.end()));
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto range = expected[index];
        EXPECT_EQ(actual[index],
                  std::vector<std::byte>(source.begin() + range.offset,
                                         source.begin() + range.offset + range.size));
    }
}

TEST(Store, PutFileMatchesPutAndStreamsBack) {
    temporary_store source_root;
    temporary_store memory_root;
    temporary_store file_root;
    const auto source = bytes(8U * 1024U * 1024U + 123U, 25);
    const auto source_path = source_root.path() / "source.bin";
    const auto output_path = source_root.path() / "output.bin";
    write_bytes(source_path, source);
    auto memory_store = open_store(memory_root.path());
    const auto memory_id = liquid::runtime::block_on(memory_store.put(source));
    ASSERT_TRUE(memory_id) << memory_id.error().message;
    memory_store = {};
    auto file_store = open_store(file_root.path());
    const auto file_id = liquid::runtime::block_on(file_store.put_file(source_path));
    ASSERT_TRUE(file_id) << file_id.error().message;
    EXPECT_EQ(liquid::to_string(*file_id), liquid::to_string(*memory_id));
    file_store = {};
    EXPECT_EQ(read_bytes(memory_root.path() / "packs" / "objects.meta"),
              read_bytes(file_root.path() / "packs" / "objects.meta"));
    file_store = open_store(file_root.path());
    const auto exported = liquid::runtime::block_on(file_store.get_to_file(*file_id, output_path));
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(read_bytes(output_path), read_bytes(source_path));
}

TEST(Store, EncryptedPutFileMatchesPutChunkList) {
    temporary_store source_root;
    temporary_store memory_root;
    temporary_store file_root;
    const auto source = bytes(8U * 1024U * 1024U + 123U, 26);
    const auto source_path = source_root.path() / "source.bin";
    write_bytes(source_path, source);
    auto memory_keys = keys_for("private", "streaming passphrase");
    auto memory_store = open_store(memory_root.path(), encrypted_options(memory_keys));
    const auto memory_id = liquid::runtime::block_on(memory_store.put(source));
    ASSERT_TRUE(memory_id) << memory_id.error().message;
    memory_store = {};
    std::filesystem::create_directories(file_root.path());
    std::filesystem::copy(memory_root.path() / "keys",
                          file_root.path() / "keys",
                          std::filesystem::copy_options::recursive);
    auto file_keys = keys_for("private", "streaming passphrase");
    auto file_store = open_store(file_root.path(), encrypted_options(file_keys));
    const auto file_id = liquid::runtime::block_on(file_store.put_file(source_path));
    ASSERT_TRUE(file_id) << file_id.error().message;
    EXPECT_EQ(liquid::to_string(*file_id), liquid::to_string(*memory_id));
    file_store = {};
    EXPECT_EQ(read_bytes(memory_root.path() / "packs" / "objects.meta"),
              read_bytes(file_root.path() / "packs" / "objects.meta"));
}

TEST(Store, HoldsBlake3AndSha256ObjectsAcrossReopen) {
    temporary_store temporary;
    const auto blake3_source = bytes(4096, 17);
    const auto sha256_source = bytes(8192, 18);

    auto store = open_store(temporary.path());
    const auto blake3_id = liquid::runtime::block_on(store.put(blake3_source));
    ASSERT_TRUE(blake3_id) << blake3_id.error().message;
    EXPECT_EQ(blake3_id->alg, liquid::algorithm::blake3);
    store = {};

    liquid::store_options sha256_store_options;
    sha256_store_options.hash_algorithm = liquid::algorithm::sha256;
    store = open_store(temporary.path(), sha256_store_options);
    const auto sha256_id = liquid::runtime::block_on(store.put(sha256_source));
    ASSERT_TRUE(sha256_id) << sha256_id.error().message;
    EXPECT_EQ(sha256_id->alg, liquid::algorithm::sha256);

    auto blake3 = liquid::runtime::block_on(store.get(*blake3_id));
    auto sha256 = liquid::runtime::block_on(store.get(*sha256_id));
    ASSERT_TRUE(blake3) << blake3.error().message;
    ASSERT_TRUE(sha256) << sha256.error().message;
    EXPECT_EQ(*blake3, blake3_source);
    EXPECT_EQ(*sha256, sha256_source);

    store = {};
    auto reopened = open_store(temporary.path(), sha256_store_options);
    blake3 = liquid::runtime::block_on(reopened.get(*blake3_id));
    sha256 = liquid::runtime::block_on(reopened.get(*sha256_id));
    ASSERT_TRUE(blake3) << blake3.error().message;
    ASSERT_TRUE(sha256) << sha256.error().message;
    EXPECT_EQ(*blake3, blake3_source);
    EXPECT_EQ(*sha256, sha256_source);
}

TEST(Store, PutGetReadDeduplicateAndReopen) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(10U * 1024U * 1024U, 7);
    auto first = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    auto got = liquid::runtime::block_on(store.get(*first));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);

    std::vector<std::byte> range(1024U * 1024U);
    auto read = liquid::runtime::block_on(store.read(*first, 65536 - 100, range));
    ASSERT_TRUE(read.has_value()) << read.error().message;
    EXPECT_EQ(*read, range.size());
    EXPECT_TRUE(std::equal(range.begin(), range.end(), source.begin() + 65536 - 100));
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    const auto before = std::filesystem::file_size(pack);
    auto second = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
    EXPECT_EQ(before, std::filesystem::file_size(pack));
    store = {};
    auto reopened = open_store(temporary.path());
    got = liquid::runtime::block_on(reopened.get(*first));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

TEST(Store, SealedPackReopensFromItsFooter) {
    temporary_store temporary;
    liquid::store_options options;
    options.pack_size = 1;
    auto store = liquid::runtime::block_on(liquid::store::open(temporary.path(), options));
    ASSERT_TRUE(store.has_value()) << store.error().message;
    const auto source = bytes(4096, 45);
    auto id = liquid::runtime::block_on(store->put(source));
    ASSERT_TRUE(id.has_value()) << id.error().message;
    *store = {};
    auto reopened = open_store(temporary.path());
    auto got = liquid::runtime::block_on(reopened.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

TEST(Store, RebuildsIndexAndReportsPackCorruption) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(256U * 1024U, 12);
    auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    auto rebuilt = open_store(temporary.path());
    auto got = liquid::runtime::block_on(rebuilt.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
    rebuilt = {};

    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    std::fstream damaged(pack, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(damaged.good());
    damaged.seekg(20);
    char changed = 0;
    damaged.read(&changed, 1);
    damaged.clear();
    damaged.seekp(20);
    changed ^= 0x7f;
    damaged.write(&changed, 1);
    damaged.close();
    auto corrupt = open_store(temporary.path());
    auto failed = liquid::runtime::block_on(corrupt.get(*id));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, liquid::errc::corrupt);
}

TEST(Store, RefusesUnknownFormatAndRebuildsBadRun) {
    temporary_store unknown;
    std::filesystem::create_directories(unknown.path());
    {
        std::ofstream marker(unknown.path() / "LIQUID_STORE");
        marker << "liquid-store 99\n";
    }
    auto bad = liquid::runtime::block_on(liquid::store::open(unknown.path()));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, liquid::errc::unsupported);

    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto source = bytes(4096, 3);
    auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    store = {};
    const auto run = temporary.path() / "index" / "run-1.idx";
    ASSERT_TRUE(std::filesystem::exists(run));
    std::fstream broken(run, std::ios::in | std::ios::out | std::ios::binary);
    broken.seekp(0);
    broken.put('X');
    broken.close();
    auto reopened = open_store(temporary.path());
    auto got = liquid::runtime::block_on(reopened.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

TEST(Store, EncryptsDeduplicatesAndKeepsPlaintextOutOfPacks) {
    temporary_store temporary;
    auto keys = keys_for("private", "correct horse battery staple");
    auto store = open_store(temporary.path(), encrypted_options(keys));
    const std::string marker = "liquid encrypted test marker: not pack plaintext";
    std::vector<std::byte> source(
        reinterpret_cast<const std::byte*>(marker.data()),
        reinterpret_cast<const std::byte*>(marker.data() + marker.size()));
    source.resize(256U * 1024U, std::byte{'x'});
    const auto first = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(first->alg, liquid::algorithm::blake3_keyed);
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    const auto before = std::filesystem::file_size(pack);
    const auto second = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(liquid::to_string(*first), liquid::to_string(*second));
    EXPECT_EQ(before, std::filesystem::file_size(pack));
    std::ifstream input(pack, std::ios::binary);
    const std::string physical((std::istreambuf_iterator<char>(input)), {});
    EXPECT_EQ(physical.find(marker), std::string::npos);
    const auto got = liquid::runtime::block_on(store.get(*first));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

TEST(Store, EncryptsIndependentlyPerTrustDomain) {
    temporary_store temporary;
    auto keys = keys_for("alpha", "alpha passphrase");
    ASSERT_TRUE(keys->add("beta", std::make_shared<liquid::passphrase_provider>("beta passphrase"))
                    .has_value());
    auto store = open_store(temporary.path(), encrypted_options(keys, "alpha"));
    const auto source = bytes(512U * 1024U, 81);
    const auto alpha = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(alpha.has_value());
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    const auto alpha_size = std::filesystem::file_size(pack);
    store = {};
    store = open_store(temporary.path(), encrypted_options(keys, "beta"));
    const auto beta = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(beta.has_value());
    EXPECT_NE(liquid::to_string(*alpha), liquid::to_string(*beta));
    EXPECT_GT(std::filesystem::file_size(pack), alpha_size);
    const auto first = liquid::runtime::block_on(store.get(*alpha));
    const auto second = liquid::runtime::block_on(store.get(*beta));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(*first, source);
    EXPECT_EQ(*second, source);
}

TEST(Store, EncryptedFailuresAreUnauthorizedOrCorrupt) {
    temporary_store temporary;
    auto good_keys = keys_for("private", "known passphrase");
    auto store = open_store(temporary.path(), encrypted_options(good_keys));
    const auto source = bytes(256U * 1024U, 19);
    const auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    store = {};
    auto plaintext = open_store(temporary.path());
    const auto absent_key = liquid::runtime::block_on(plaintext.get(*id));
    ASSERT_FALSE(absent_key.has_value());
    EXPECT_EQ(absent_key.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(absent_key.error().context, "private@1");
    plaintext = {};
    auto wrong_keys = keys_for("private", "wrong passphrase");
    auto wrong = open_store(temporary.path(), encrypted_options(wrong_keys));
    const auto wrong_key = liquid::runtime::block_on(wrong.get(*id));
    ASSERT_FALSE(wrong_key.has_value());
    EXPECT_EQ(wrong_key.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(wrong_key.error().context, "private@1");
    wrong = {};
    auto reopened = open_store(temporary.path(), encrypted_options(good_keys));
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    std::fstream damaged(pack, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(damaged.good());
    damaged.seekg(70);
    char changed = 0;
    damaged.read(&changed, 1);
    damaged.clear();
    damaged.seekp(70);
    damaged.put(static_cast<char>(changed ^ 0x7f));
    damaged.close();
    const auto corrupt = liquid::runtime::block_on(reopened.get(*id));
    ASSERT_FALSE(corrupt.has_value());
    EXPECT_EQ(corrupt.error().code, liquid::errc::corrupt);
}

TEST(Store, RotationVersionsFutureWritesWithoutRewritingPacks) {
    temporary_store temporary;
    auto keys = keys_for("private", "rotation passphrase");
    auto store = open_store(temporary.path(), encrypted_options(keys));
    const auto old_source = bytes(256U * 1024U, 91);
    const auto new_source = bytes(256U * 1024U, 92);
    const auto old_id = liquid::runtime::block_on(store.put(old_source));
    ASSERT_TRUE(old_id) << old_id.error().message;
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    std::ifstream before_input(pack, std::ios::binary);
    const std::vector<char> before((std::istreambuf_iterator<char>(before_input)), {});

    const auto rotated = liquid::runtime::block_on(keys->rotate("private"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated->key_id, "private");
    EXPECT_EQ(rotated->previous_version, 1U);
    EXPECT_EQ(rotated->new_version, 2U);
    ASSERT_EQ(rotated->limits.size(), 3U);
    EXPECT_EQ(rotated->limits[0], "Rotation does not rewrite history.");
    std::ifstream after_input(pack, std::ios::binary);
    const std::vector<char> after((std::istreambuf_iterator<char>(after_input)), {});
    EXPECT_EQ(after, before);

    const auto new_id = liquid::runtime::block_on(store.put(new_source));
    ASSERT_TRUE(new_id) << new_id.error().message;
    const auto old_version = liquid::runtime::block_on(store.key_version(*old_id));
    const auto new_version = liquid::runtime::block_on(store.key_version(*new_id));
    ASSERT_TRUE(old_version);
    ASSERT_TRUE(new_version);
    ASSERT_TRUE(*old_version);
    ASSERT_TRUE(*new_version);
    EXPECT_EQ((*old_version)->key_id, "private");
    EXPECT_EQ((*old_version)->version, 1U);
    EXPECT_TRUE((*old_version)->readable);
    EXPECT_EQ((*new_version)->key_id, "private");
    EXPECT_EQ((*new_version)->version, 2U);
    EXPECT_TRUE((*new_version)->readable);
    auto old_value = liquid::runtime::block_on(store.get(*old_id));
    auto new_value = liquid::runtime::block_on(store.get(*new_id));
    ASSERT_TRUE(old_value) << old_value.error().message;
    ASSERT_TRUE(new_value) << new_value.error().message;
    EXPECT_EQ(*old_value, old_source);
    EXPECT_EQ(*new_value, new_source);

    const auto revoked = liquid::runtime::block_on(keys->revoke("private", 1));
    ASSERT_TRUE(revoked) << revoked.error().message;
    old_value = liquid::runtime::block_on(store.get(*old_id));
    new_value = liquid::runtime::block_on(store.get(*new_id));
    ASSERT_FALSE(old_value);
    EXPECT_EQ(old_value.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(old_value.error().context, "private@1");
    ASSERT_TRUE(new_value) << new_value.error().message;
    EXPECT_EQ(*new_value, new_source);
    const auto revoked_version = liquid::runtime::block_on(store.key_version(*old_id));
    ASSERT_TRUE(revoked_version);
    ASSERT_TRUE(*revoked_version);
    EXPECT_FALSE((*revoked_version)->readable);
}

TEST(Store, RotationWithWrongKekChangesNoFiles) {
    temporary_store temporary;
    auto good_keys = keys_for("private", "correct passphrase");
    auto store = open_store(temporary.path(), encrypted_options(good_keys));
    const auto id = liquid::runtime::block_on(store.put(bytes(256U * 1024U, 93)));
    ASSERT_TRUE(id) << id.error().message;
    store = {};
    const auto key_path = temporary.path() / "keys" / "private.wrapped";
    const auto pack_path = temporary.path() / "packs" / "pack-1.pack";
    std::ifstream key_before_input(key_path, std::ios::binary);
    const std::vector<char> key_before((std::istreambuf_iterator<char>(key_before_input)), {});
    std::ifstream pack_before_input(pack_path, std::ios::binary);
    const std::vector<char> pack_before((std::istreambuf_iterator<char>(pack_before_input)), {});

    auto wrong_keys = keys_for("private", "wrong passphrase");
    store = open_store(temporary.path(), encrypted_options(wrong_keys));
    const auto rotated = liquid::runtime::block_on(wrong_keys->rotate("private"));
    ASSERT_FALSE(rotated);
    EXPECT_EQ(rotated.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(rotated.error().context, "private@1");
    std::ifstream key_after_input(key_path, std::ios::binary);
    const std::vector<char> key_after((std::istreambuf_iterator<char>(key_after_input)), {});
    std::ifstream pack_after_input(pack_path, std::ios::binary);
    const std::vector<char> pack_after((std::istreambuf_iterator<char>(pack_after_input)), {});
    EXPECT_EQ(key_after, key_before);
    EXPECT_EQ(pack_after, pack_before);
}

TEST(Store, EncryptedRangeReadsAndKekRewrapDoNotRewritePacks) {
    temporary_store temporary;
    auto keys = keys_for("private", "before rotation");
    auto store = open_store(temporary.path(), encrypted_options(keys));
    const auto source = bytes(10U * 1024U * 1024U, 73);
    const auto id = liquid::runtime::block_on(store.put(source));
    ASSERT_TRUE(id.has_value());
    std::vector<std::byte> range(8192);
    const auto read = liquid::runtime::block_on(store.read(*id, 65536 - 100, range));
    ASSERT_TRUE(read.has_value()) << read.error().message;
    EXPECT_EQ(*read, range.size());
    EXPECT_TRUE(std::equal(range.begin(), range.end(), source.begin() + 65536 - 100));
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    std::ifstream before_input(pack, std::ios::binary);
    const std::vector<char> before((std::istreambuf_iterator<char>(before_input)), {});
    const auto rewrapped = liquid::runtime::block_on(
        keys->rewrap("private", std::make_shared<liquid::passphrase_provider>("after rotation")));
    ASSERT_TRUE(rewrapped.has_value()) << rewrapped.error().message;
    std::ifstream after_input(pack, std::ios::binary);
    const std::vector<char> after((std::istreambuf_iterator<char>(after_input)), {});
    EXPECT_EQ(before, after);
    store = {};
    auto reloaded_keys = keys_for("private", "after rotation");
    auto reopened = open_store(temporary.path(), encrypted_options(reloaded_keys));
    const auto got = liquid::runtime::block_on(reopened.get(*id));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, source);
}

} // namespace
