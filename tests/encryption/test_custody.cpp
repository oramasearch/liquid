#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "crypto/crypto.hpp"
#include "identity/custody/age.hpp"
#include "identity/custody/common.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "monocypher.h"

namespace {

constexpr auto legacy_context = std::string_view{"liquid store data key wrapping v1"};

class temporary_store {
public:
    temporary_store()
        : path_(liquid::test::fixture_path("liquid-custody-test-" + std::to_string(++counter_))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~temporary_store() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t counter_{};
    std::filesystem::path path_;
};

auto put_u32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    for (auto index = 0U; index != 4U; ++index)
        output.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

auto put_u64(std::vector<std::byte>& output, std::uint64_t value) -> void {
    for (auto index = 0U; index != 8U; ++index)
        output.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

auto write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) -> void {
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

auto read_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    auto output = std::vector<std::byte>{};
    for (char value; input.get(value);)
        output.push_back(std::byte{static_cast<unsigned char>(value)});
    return output;
}

class scoped_environment {
public:
    scoped_environment(std::string name, std::string value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str()))
            previous_ = previous;
        set(value);
    }

    ~scoped_environment() {
        if (previous_.empty()) {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        } else {
            set(previous_);
        }
    }

private:
    auto set(const std::string& value) -> void {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    std::string previous_;
};

class test_provider final : public liquid::key_provider {
public:
    test_provider(std::string kind, std::byte marker) : kind_(std::move(kind)), wrapping_{} {
        wrapping_.fill(marker);
    }

    auto wrap(std::span<const std::byte, 32> data_key, liquid::wrap_context context)
        -> liquid::runtime::task<std::vector<std::byte>> override {
        co_return liquid::crypto_detail::seal(
            wrapping_, liquid::custody_detail::as_bytes(reference(context)), data_key);
    }

    auto unwrap(std::span<const std::byte> wrapped, liquid::wrap_context context)
        -> liquid::runtime::task<liquid::key> override {
        auto opened = liquid::crypto_detail::open(
            wrapping_, liquid::custody_detail::as_bytes(reference(context)), wrapped);
        if (!opened || opened->size() != liquid::key{}.size()) {
            co_return std::unexpected(liquid::error{
                liquid::errc::unauthorized, "test custody rejected key", reference(context)});
        }
        auto output = liquid::key{};
        std::ranges::copy(*opened, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return kind_;
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return {};
    }

private:
    static auto reference(const liquid::wrap_context& context) -> std::string {
        return context.key_id + "@" + std::to_string(context.version);
    }

    std::string kind_;
    liquid::key wrapping_;
};

auto provider_set(std::string kind, std::byte marker) -> std::shared_ptr<liquid::keyring> {
    auto output = std::make_shared<liquid::keyring>();
    EXPECT_TRUE(output->add("private", std::make_shared<test_provider>(std::move(kind), marker)));
    EXPECT_TRUE(output->set_default_key_id("private"));
    return output;
}

auto encrypted_options(std::shared_ptr<liquid::keyring> keys) -> liquid::store_options {
    auto output = liquid::store_options{};
    output.keys = std::move(keys);
    output.write_key = "private";
    return output;
}

auto legacy_file(unsigned format,
                 liquid::passphrase_provider& provider,
                 const liquid::key& data_key) -> std::vector<std::byte> {
    auto salt = std::array<std::byte, 16>{};
    salt[0] = std::byte{0x51};
    const auto parameters = liquid::argon2_parameters{8, 3, 1};
    auto kek = provider.derive(salt, parameters);
    EXPECT_TRUE(kek);
    auto wrapping = liquid::crypto_detail::derive_key(*kek, legacy_context);
    EXPECT_TRUE(wrapping);
    auto boxed = liquid::crypto_detail::seal(*wrapping, {}, data_key);
    EXPECT_TRUE(boxed);
    auto output = std::vector<std::byte>{std::byte{'L'},
                                         std::byte{'Q'},
                                         std::byte{'K'},
                                         std::byte{'E'},
                                         std::byte{'Y'},
                                         std::byte{static_cast<unsigned char>(format)},
                                         std::byte{},
                                         std::byte{}};
    output.insert(output.end(), salt.begin(), salt.end());
    put_u32(output, parameters.memory_kib);
    put_u32(output, parameters.iterations);
    put_u32(output, parameters.lanes);
    if (format == 2) {
        put_u64(output, 1);
        put_u32(output, 1);
        put_u32(output, 0);
        put_u64(output, 1);
    }
    output.insert(output.end(), boxed->begin(), boxed->end());
    return output;
}

auto bech32_polymod(std::span<const std::uint8_t> values) -> std::uint32_t {
    constexpr auto generators = std::array<std::uint32_t, 5>{
        0x3b6a57b2U, 0x26508e6dU, 0x1ea119faU, 0x3d4233ddU, 0x2a1462b3U};
    auto checksum = std::uint32_t{1};
    for (const auto value : values) {
        const auto top = checksum >> 25U;
        checksum = (checksum & 0x1ffffffU) << 5U ^ value;
        for (auto index = 0U; index != generators.size(); ++index) {
            if (((top >> index) & 1U) != 0)
                checksum ^= generators[index];
        }
    }
    return checksum;
}

auto bech32(std::string_view prefix, std::span<const std::byte> input, bool upper) -> std::string {
    constexpr auto alphabet = std::string_view{"qpzry9x8gf2tvdw0s3jn54khce6mua7l"};
    auto values = std::vector<std::uint8_t>{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto octet : input) {
        accumulator = accumulator << 8U | std::to_integer<std::uint8_t>(octet);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            values.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 31U));
        }
    }
    if (bits != 0)
        values.push_back(static_cast<std::uint8_t>((accumulator << (5U - bits)) & 31U));
    auto expanded = std::vector<std::uint8_t>{};
    for (const auto character : prefix)
        expanded.push_back(static_cast<std::uint8_t>(character >> 5U));
    expanded.push_back(0);
    for (const auto character : prefix)
        expanded.push_back(static_cast<std::uint8_t>(character & 31U));
    expanded.insert(expanded.end(), values.begin(), values.end());
    expanded.insert(expanded.end(), 6, 0);
    const auto checksum = bech32_polymod(expanded) ^ 1U;
    auto output = std::string{prefix} + "1";
    for (const auto value : values)
        output.push_back(alphabet[value]);
    for (auto index = 0U; index != 6U; ++index)
        output.push_back(alphabet[(checksum >> (5U * (5U - index))) & 31U]);
    if (upper) {
        std::ranges::transform(output, output.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    }
    return output;
}

auto fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_SOURCE_DIR} / "tests" / "fixtures" / "age" /
                      (std::string{name} + ".b64");
    auto input = std::ifstream{path};
    const auto encoded = std::string{std::istreambuf_iterator<char>{input}, {}};
    auto decoded =
        liquid::custody_detail::base64_decode(encoded.substr(0, encoded.find('\n')), false, true);
    EXPECT_TRUE(decoded);
    return decoded ? std::move(*decoded) : std::vector<std::byte>{};
}

} // namespace

TEST(Custody, PassphraseWrapRoundTripBindsTheKeyVersion) {
    auto provider = liquid::passphrase_provider{"correct horse battery staple"};
    auto data_key = liquid::key{};
    data_key[0] = std::byte{0x51};
    auto wrapped = liquid::runtime::block_on(provider.wrap(data_key, {"private", 7}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    auto opened = liquid::runtime::block_on(provider.unwrap(*wrapped, {"private", 7}));
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, data_key);
    auto wrong = liquid::runtime::block_on(provider.unwrap(*wrapped, {"private", 8}));
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(wrong.error().context, "private@8");
    auto wrong_provider = liquid::passphrase_provider{"incorrect passphrase"};
    wrong = liquid::runtime::block_on(wrong_provider.unwrap(*wrapped, {"private", 7}));
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(wrong.error().context, "private@7");
    auto exposed =
        liquid::runtime::block_on(liquid::key_provider_from_uri("passphrase:not-a-secret-slot"));
    ASSERT_FALSE(exposed);
    EXPECT_EQ(exposed.error().code, liquid::errc::invalid_argument);
    EXPECT_EQ(exposed.error().context, "passphrase:");
}

TEST(Custody, KeyFileWrapRoundTripUsesOnlyThePrivateFile) {
    temporary_store temporary;
    const auto path = temporary.path() / "kek";
    auto key_bytes = liquid::key{};
    key_bytes[4] = std::byte{0x51};
    write_bytes(path, key_bytes);
#if !defined(_WIN32)
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    auto provider = liquid::runtime::block_on(liquid::key_file_provider::open(path));
    ASSERT_TRUE(provider) << provider.error().message;
    auto data_key = liquid::key{};
    data_key[7] = std::byte{0x22};
    auto wrapped = liquid::runtime::block_on((*provider)->wrap(data_key, {"file-key", 6}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    auto opened = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"file-key", 6}));
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, data_key);
    const auto wrong_path = temporary.path() / "wrong-kek";
    key_bytes[4] = std::byte{0x52};
    write_bytes(wrong_path, key_bytes);
#if !defined(_WIN32)
    std::filesystem::permissions(wrong_path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    auto wrong_provider = liquid::runtime::block_on(liquid::key_file_provider::open(wrong_path));
    ASSERT_TRUE(wrong_provider) << wrong_provider.error().message;
    auto refused = liquid::runtime::block_on((*wrong_provider)->unwrap(*wrapped, {"file-key", 6}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(refused.error().context, "file-key@6");
}

TEST(Custody, AgeSpecificationX25519VectorsDecrypt) {
    constexpr auto identity =
        "AGE-SECRET-KEY-1EGTZVFFV20835NWYV6270LXYVK2VKNX2MMDKWYKLMGR48UAWX40Q2P2LM0";
    constexpr auto expected = "616765";
    for (const auto name : {"x25519", "x25519-multiple-recipients"}) {
        auto vector = fixture(name);
        constexpr auto separator = std::array{std::byte{'\n'}, std::byte{'\n'}};
        const auto marker =
            std::search(vector.begin(), vector.end(), separator.begin(), separator.end());
        ASSERT_NE(marker, vector.end());
        const auto age = std::span<const std::byte>{marker + 2, vector.end()};
        const auto identities = std::array<std::string_view, 1>{identity};
        auto opened = liquid::custody_detail::age_decrypt(age, identities);
        ASSERT_TRUE(opened) << opened.error().message;
        EXPECT_EQ(liquid::custody_detail::hex(*opened), expected);
    }
}

TEST(Custody, AgeProviderWrapsForAnX25519Recipient) {
    auto secret = liquid::key{};
    secret[0] = std::byte{7};
    auto public_key = liquid::key{};
    crypto_x25519_public_key(reinterpret_cast<std::uint8_t*>(public_key.data()),
                             reinterpret_cast<const std::uint8_t*>(secret.data()));
    const auto recipient = bech32("age", public_key, false);
    auto other_secret = liquid::key{};
    other_secret[0] = std::byte{13};
    auto other_public_key = liquid::key{};
    crypto_x25519_public_key(reinterpret_cast<std::uint8_t*>(other_public_key.data()),
                             reinterpret_cast<const std::uint8_t*>(other_secret.data()));
    const auto other_recipient = bech32("age", other_public_key, false);
    const auto other_identity = bech32("age-secret-key-", other_secret, true);
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("age:" + recipient + "," + other_recipient));
    ASSERT_TRUE(provider) << provider.error().message;
    auto data_key = liquid::key{};
    data_key[3] = std::byte{9};
    auto wrapped = liquid::runtime::block_on((*provider)->wrap(data_key, {"age-key", 2}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    {
        auto environment = scoped_environment{"LIQUID_AGE_IDENTITIES", other_identity};
        auto opened = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"age-key", 2}));
        ASSERT_TRUE(opened) << opened.error().message;
        EXPECT_EQ(*opened, data_key);
    }
    {
        auto environment = scoped_environment{"LIQUID_AGE_IDENTITIES", "not-an-age-identity"};
        auto refused = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"age-key", 2}));
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
        EXPECT_EQ(refused.error().context, "age-key@2");
    }
}

TEST(Custody, StoreOptionsAcceptCustodyUris) {
    temporary_store temporary;
    auto passphrase = scoped_environment{"LIQUID_PASSPHRASE", "store options passphrase"};
    auto options = liquid::store_options{};
    options.custody.emplace("private", "passphrase:");
    options.write_key = "private";
    auto opened = liquid::runtime::block_on(liquid::store::open(temporary.path(), options));
    ASSERT_TRUE(opened) << opened.error().message;
    const auto source = std::vector<std::byte>{std::byte{0x51}};
    auto id = liquid::runtime::block_on(opened->put(source));
    ASSERT_TRUE(id) << id.error().message;
    auto value = liquid::runtime::block_on(opened->get(*id));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(*value, source);
}

TEST(Custody, ShamirRequiresTheConfiguredThreshold) {
    auto split = liquid::split_custody(2, 3);
    ASSERT_TRUE(split) << split.error().message;
    ASSERT_EQ(split->shares.size(), 3U);
    auto data_key = liquid::key{};
    data_key[5] = std::byte{0x33};
    auto wrapped = liquid::runtime::block_on(split->provider->wrap(data_key, {"shared", 4}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    {
        temporary_store temporary;
        const auto share_file = temporary.path() / "share.txt";
        auto share_bytes = std::span{reinterpret_cast<const std::byte*>(split->shares[0].data()),
                                     split->shares[0].size()};
        write_bytes(share_file, share_bytes);
        auto shares = scoped_environment{"LIQUID_SHAMIR_SHARES",
                                         "@" + share_file.string() + "," + split->shares[2]};
        auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri("shamir:2/3"));
        ASSERT_TRUE(provider) << provider.error().message;
        auto opened = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"shared", 4}));
        ASSERT_TRUE(opened) << opened.error().message;
        EXPECT_EQ(*opened, data_key);
    }
    {
        auto shares = scoped_environment{"LIQUID_SHAMIR_SHARES", split->shares[0]};
        auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri("shamir:2/3"));
        ASSERT_TRUE(provider) << provider.error().message;
        auto refused = liquid::runtime::block_on((*provider)->unwrap(*wrapped, {"shared", 4}));
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
        EXPECT_EQ(refused.error().context, "shared@4");
    }
}

TEST(Custody, V1V2AndV3WrappedKeyFilesRemainCompatible) {
    for (const auto format : {1U, 2U}) {
        temporary_store temporary;
        auto provider = std::make_shared<liquid::passphrase_provider>("legacy passphrase");
        auto keys = std::make_shared<liquid::keyring>();
        ASSERT_TRUE(keys->add("private", provider));
        ASSERT_TRUE(keys->set_default_key_id("private"));
        auto data_key = liquid::key{};
        data_key[0] = std::byte{static_cast<unsigned char>(format)};
        write_bytes(temporary.path() / "keys" / "private.wrapped",
                    legacy_file(format, *provider, data_key));
        auto opened = liquid::runtime::block_on(
            liquid::store::open(temporary.path(), encrypted_options(keys)));
        ASSERT_TRUE(opened) << opened.error().message;
        auto source = std::vector<std::byte>(4096, std::byte{0x51});
        auto id = liquid::runtime::block_on(opened->put(source));
        ASSERT_TRUE(id) << id.error().message;
        auto value = liquid::runtime::block_on(opened->get(*id));
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_EQ(*value, source);
        auto rotated = liquid::runtime::block_on(keys->rotate("private"));
        ASSERT_TRUE(rotated) << rotated.error().message;
        const auto migrated = read_bytes(temporary.path() / "keys" / "private.wrapped");
        ASSERT_GE(migrated.size(), 8U);
        EXPECT_EQ(migrated[5], std::byte{3});
    }

    temporary_store temporary;
    auto keys = provider_set("passphrase", std::byte{7});
    auto opened =
        liquid::runtime::block_on(liquid::store::open(temporary.path(), encrypted_options(keys)));
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(opened->put(std::vector<std::byte>{std::byte{1}})));
    auto records = liquid::runtime::block_on(keys->show("private"));
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records->size(), 1U);
    EXPECT_EQ(records->front().custody, "passphrase");
}

TEST(Custody, EveryCustodyPairRewrapsWithoutChangingPackBytes) {
    constexpr auto kinds = std::array<std::string_view, 8>{
        "passphrase", "file", "age", "aws-kms", "gcp-kms", "azure-kv", "lake", "shamir"};
    for (auto from = std::size_t{}; from != kinds.size(); ++from) {
        for (auto to = std::size_t{}; to != kinds.size(); ++to) {
            temporary_store temporary;
            auto keys = provider_set(std::string{kinds[from]}, std::byte{0x11});
            auto opened = liquid::runtime::block_on(
                liquid::store::open(temporary.path(), encrypted_options(keys)));
            ASSERT_TRUE(opened) << opened.error().message;
            auto source = std::vector<std::byte>(4096, std::byte{0x44});
            auto id = liquid::runtime::block_on(opened->put(source));
            ASSERT_TRUE(id) << id.error().message;
            const auto pack = temporary.path() / "packs" / "pack-1.pack";
            const auto before = read_bytes(pack);
            auto changed = liquid::runtime::block_on(keys->rewrap(
                "private",
                std::make_shared<test_provider>(std::string{kinds[to]}, std::byte{0x22})));
            ASSERT_TRUE(changed) << kinds[from] << " -> " << kinds[to] << ": "
                                 << changed.error().message;
            EXPECT_EQ(read_bytes(pack), before) << kinds[from] << " -> " << kinds[to];
            auto value = liquid::runtime::block_on(opened->get(*id));
            ASSERT_TRUE(value) << kinds[from] << " -> " << kinds[to] << ": "
                               << value.error().message;
            EXPECT_EQ(*value, source);
        }
    }
}
