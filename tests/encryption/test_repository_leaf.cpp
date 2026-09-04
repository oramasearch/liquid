#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "formats/json.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-leaf-repository-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view input) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(input.data()),
            reinterpret_cast<const std::byte*>(input.data() + input.size())};
}

auto text(const std::vector<std::byte>& input) -> std::string {
    return {reinterpret_cast<const char*>(input.data()), input.size()};
}

auto configured_options(liquid::encryption_path_rule rule)
    -> std::pair<liquid::repository_options, std::shared_ptr<liquid::keyring>> {
    auto keys = std::make_shared<liquid::keyring>();
    auto provider = std::make_shared<liquid::passphrase_provider>("repository-leaf-passphrase");
    EXPECT_TRUE(keys->add("team", provider));
    EXPECT_TRUE(keys->set_default_key_id("team"));
    liquid::repository_options options;
    options.store.keys = keys;
    options.encrypt.paths.push_back(std::move(rule));
    return {std::move(options), std::move(keys)};
}

auto file_contains(const std::filesystem::path& root, std::string_view needle) -> bool {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto input = std::ifstream{entry.path(), std::ios::binary | std::ios::ate};
        const auto length = input.tellg();
        if (length < 0) {
            continue;
        }
        auto content = std::string(static_cast<std::size_t>(length), '\0');
        input.seekg(0);
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(RepositoryLeafEncryption, RulesPersistAndExplicitDecryptionRoundTrips) {
    temporary_repository location;
    auto [options, keys] = configured_options(
        {"config/*.json", "team", liquid::encryption_mode::leaf, "(password|token)$"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    constexpr auto source = R"({"database":{"user":"reader","password":"secret"},"token":"first"})";
    auto written = liquid::runtime::block_on(repository.write("config/app.json", bytes(source)));
    ASSERT_TRUE(written) << written.error().message;

    auto ciphertext = liquid::runtime::block_on(repository.read("config/app.json"));
    ASSERT_TRUE(ciphertext) << ciphertext.error().message;
    EXPECT_NE(text(*ciphertext).find("\"user\": \"reader\""), std::string::npos);
    EXPECT_EQ(text(*ciphertext).find("\"password\": \"secret\""), std::string::npos);
    EXPECT_NE(text(*ciphertext).find("ENC[liquid:v1:team@1:"), std::string::npos);

    auto plaintext = liquid::runtime::block_on(repository.read_decrypted("config/app.json"));
    ASSERT_TRUE(plaintext) << plaintext.error().message;
    auto expected = liquid::formats::json::parse(source);
    ASSERT_TRUE(expected);
    auto expected_text = liquid::formats::json::write(*expected);
    ASSERT_TRUE(expected_text);
    EXPECT_EQ(text(*plaintext), *expected_text);

    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    ciphertext = liquid::runtime::block_on(reopened->read("config/app.json"));
    ASSERT_TRUE(ciphertext) << ciphertext.error().message;
    EXPECT_NE(text(*ciphertext).find("ENC[liquid:v1:team@1:"), std::string::npos);
    plaintext = liquid::runtime::block_on(reopened->read_decrypted("config/app.json"));
    ASSERT_FALSE(plaintext);
    EXPECT_EQ(plaintext.error().code, liquid::errc::unauthorized);
}

TEST(RepositoryLeafEncryption, PlaintextWriteWithoutTheDeclaredKeyIsRefused) {
    temporary_repository location;
    liquid::repository_options options;
    options.encrypt.paths.push_back(
        {"secrets.json", "team", liquid::encryption_mode::leaf, "token$"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto refused = liquid::runtime::block_on(
        created->write("secrets.json", bytes(R"({"token":"plaintext"})")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}

TEST(RepositoryLeafEncryption, FileWriteAppliesTheDeclaredLeafRule) {
    temporary_repository location;
    auto [options, keys] =
        configured_options({"config/*.json", "team", liquid::encryption_mode::leaf, "token$"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    const auto source_path = location.path() / "source.json";
    {
        auto output = std::ofstream{source_path, std::ios::binary};
        ASSERT_TRUE(output);
        output << R"({"visible":"yes","token":"private"})";
        ASSERT_TRUE(output);
    }

    auto written =
        liquid::runtime::block_on(created->write_file("config/from-file.json", source_path));
    ASSERT_TRUE(written) << written.error().message;
    auto ciphertext = liquid::runtime::block_on(created->read("config/from-file.json"));
    ASSERT_TRUE(ciphertext) << ciphertext.error().message;
    EXPECT_NE(text(*ciphertext).find("\"visible\": \"yes\""), std::string::npos);
    EXPECT_EQ(text(*ciphertext).find("\"token\": \"private\""), std::string::npos);
    EXPECT_NE(text(*ciphertext).find("ENC[liquid:v1:team@1:"), std::string::npos);
}

TEST(RepositoryLeafEncryption, RotationKeepsOldLeavesReadableAndUsesCurrentVersion) {
    temporary_repository location;
    auto [options, keys] = configured_options(
        {"config.json", "team", liquid::encryption_mode::leaf, "(password|token)$"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    auto written = liquid::runtime::block_on(
        repository.write("config.json", bytes(R"({"password":"same","token":"first"})")));
    ASSERT_TRUE(written) << written.error().message;
    auto rotated = liquid::runtime::block_on(keys->rotate("team"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated->new_version, 2U);

    written = liquid::runtime::block_on(
        repository.write("config.json", bytes(R"({"password":"same","token":"second"})")));
    ASSERT_TRUE(written) << written.error().message;
    auto ciphertext = liquid::runtime::block_on(repository.read("config.json"));
    ASSERT_TRUE(ciphertext) << ciphertext.error().message;
    EXPECT_NE(text(*ciphertext).find("ENC[liquid:v1:team@1:"), std::string::npos);
    EXPECT_NE(text(*ciphertext).find("ENC[liquid:v1:team@2:"), std::string::npos);
    auto plaintext = liquid::runtime::block_on(repository.read_decrypted("config.json"));
    ASSERT_TRUE(plaintext) << plaintext.error().message;
    EXPECT_NE(text(*plaintext).find("\"password\": \"same\""), std::string::npos);
    EXPECT_NE(text(*plaintext).find("\"token\": \"second\""), std::string::npos);
}

TEST(RepositoryLeafEncryption, KeyDiffUsesStoredStructureWithoutDecrypting) {
    temporary_repository location;
    auto [options, keys] = configured_options(
        {"config.json", "team", liquid::encryption_mode::leaf, "(password|token)$"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write(
        "config.json", bytes(R"({"user":"reader","password":"same","token":"first"})"))));
    auto first = liquid::runtime::block_on(repository.cut("first encrypted config"));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository.write(
        "config.json", bytes(R"({"user":"reader","password":"same","token":"second"})"))));
    auto second = liquid::runtime::block_on(repository.cut("second encrypted config"));
    ASSERT_TRUE(second) << second.error().message;

    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto changed = liquid::runtime::block_on(reopened->diff_keys("config.json", *first, *second));
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(*changed, std::vector<std::string>{"/token"});
}

TEST(RepositoryLeafEncryption, FileModeUsesWholeFileStoreEncryption) {
    temporary_repository location;
    auto [options, keys] =
        configured_options({"private.bin", "team", liquid::encryption_mode::file, ".*"});
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    constexpr auto marker = "whole-file-private-marker";
    auto written = liquid::runtime::block_on(repository.write("private.bin", bytes(marker)));
    ASSERT_TRUE(written) << written.error().message;

    auto ciphertext = liquid::runtime::block_on(repository.read("private.bin"));
    ASSERT_TRUE(ciphertext) << ciphertext.error().message;
    EXPECT_TRUE(text(*ciphertext).starts_with("ENC[liquid:v1:team@1:"));
    auto plaintext = liquid::runtime::block_on(repository.read_decrypted("private.bin"));
    ASSERT_TRUE(plaintext) << plaintext.error().message;
    EXPECT_EQ(text(*plaintext), marker);
    EXPECT_FALSE(file_contains(location.path() / ".liquid-store" / "packs", marker));
}
