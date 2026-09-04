#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

class temporary_store {
public:
    temporary_store() : path_(std::filesystem::temp_directory_path() /
                              ("liquid-store-test-" + std::to_string(++counter_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_store() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned counter_{};
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::size_t size, std::uint64_t seed = 9) {
    std::vector<std::byte> output(size);
    std::mt19937_64 random(seed);
    for (auto& byte : output) byte = std::byte{static_cast<unsigned char>(random())};
    return output;
}

liquid::store open_store(const std::filesystem::path& path, liquid::store_options options = {}) {
    auto opened = liquid::runtime::block_on(liquid::store::open(path, std::move(options)));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

std::shared_ptr<liquid::keyring> keys_for(std::string id, std::string passphrase) {
    auto keys = std::make_shared<liquid::keyring>();
    EXPECT_TRUE(keys->add(id, std::make_shared<liquid::passphrase_provider>(std::move(passphrase))).has_value());
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
    { std::ofstream marker(unknown.path() / "LIQUID_STORE"); marker << "liquid-store 99\n"; }
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
    broken.seekp(0); broken.put('X'); broken.close();
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
    std::vector<std::byte> source(reinterpret_cast<const std::byte*>(marker.data()),
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
    ASSERT_TRUE(keys->add("beta", std::make_shared<liquid::passphrase_provider>("beta passphrase")).has_value());
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
    EXPECT_EQ(absent_key.error().context, "private");
    plaintext = {};
    auto wrong_keys = keys_for("private", "wrong passphrase");
    auto wrong = open_store(temporary.path(), encrypted_options(wrong_keys));
    const auto wrong_key = liquid::runtime::block_on(wrong.get(*id));
    ASSERT_FALSE(wrong_key.has_value());
    EXPECT_EQ(wrong_key.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(wrong_key.error().context, "private");
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

}  // namespace
