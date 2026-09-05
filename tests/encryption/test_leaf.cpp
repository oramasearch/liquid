#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "../temporary_directory.hpp"
#include "encryption/leaf.hpp"
#include "formats/dotenv.hpp"
#include "formats/json.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

class temporary_store {
public:
    temporary_store()
        : path_(liquid::test::fixture_path("liquid-leaf-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_store() {
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

struct key_fixture {
    temporary_store location;
    std::shared_ptr<liquid::keyring> keys{std::make_shared<liquid::keyring>()};
    liquid::store attached;

    key_fixture() {
        auto provider = std::make_shared<liquid::passphrase_provider>("leaf-test-passphrase");
        EXPECT_TRUE(keys->add("team", provider));
        EXPECT_TRUE(keys->set_default_key_id("team"));
        liquid::store_options options;
        options.keys = keys;
        auto opened = liquid::runtime::block_on(liquid::store::open(location.path(), options));
        EXPECT_TRUE(opened) << opened.error().message;
        if (opened) {
            attached = std::move(*opened);
        }
    }
};

auto object(liquid::formats::json::value& value) -> liquid::formats::json::value::object& {
    return std::get<liquid::formats::json::value::object>(value.data);
}

auto text(const liquid::formats::json::value& value) -> const std::string& {
    return std::get<std::string>(value.data);
}

} // namespace

TEST(LeafEncryption, JsonLeavesUseFreshEnvelopesAndLeaveStructureClear) {
    key_fixture fixture;
    auto parsed = liquid::formats::json::parse(
        R"({"database":{"user":"reader","password":"same"},"backup_password":"same","ports":[5432]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const liquid::encryption_detail::leaf_rule rule{"team", "password$"};

    auto encrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::encrypt_json(*parsed, rule, fixture.keys));
    ASSERT_TRUE(encrypted) << encrypted.error().message;
    auto& root = object(*encrypted);
    auto& database = object(root[0].second);
    EXPECT_EQ(text(database[0].second), "reader");
    EXPECT_TRUE(text(database[1].second).starts_with("ENC[liquid:v1:team@1:"));
    EXPECT_TRUE(text(root[1].second).starts_with("ENC[liquid:v1:team@1:"));
    EXPECT_NE(text(database[1].second), text(root[1].second));
    EXPECT_TRUE(liquid::encryption_detail::validate_json(*encrypted, rule));

    auto decrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::decrypt_json(*encrypted, rule, fixture.keys));
    ASSERT_TRUE(decrypted) << decrypted.error().message;
    EXPECT_EQ(*decrypted, *parsed);
}

TEST(LeafEncryption, AssociatedKeyPathRejectsMovedJsonValue) {
    key_fixture fixture;
    auto parsed = liquid::formats::json::parse(R"({"first":"secret"})");
    ASSERT_TRUE(parsed);
    const liquid::encryption_detail::leaf_rule rule{"team", "^/(first|second)$"};
    auto encrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::encrypt_json(*parsed, rule, fixture.keys));
    ASSERT_TRUE(encrypted);
    object(*encrypted)[0].first = "second";

    auto decrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::decrypt_json(*encrypted, rule, fixture.keys));
    ASSERT_FALSE(decrypted);
    EXPECT_EQ(decrypted.error().code, liquid::errc::corrupt);
}

TEST(LeafEncryption, DotenvCommentsAndUnselectedValuesSurvive) {
    key_fixture fixture;
    auto parsed = liquid::formats::dotenv::parse(
        "# deployment\nPUBLIC_HOST=example.test\nAPI_TOKEN=secret\n");
    ASSERT_TRUE(parsed);
    const liquid::encryption_detail::leaf_rule rule{"team", "TOKEN$"};
    auto encrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::encrypt_dotenv(*parsed, rule, fixture.keys));
    ASSERT_TRUE(encrypted) << encrypted.error().message;
    EXPECT_EQ(encrypted->lines[0].text, "# deployment");
    EXPECT_EQ(encrypted->lines[1].value, "example.test");
    EXPECT_TRUE(encrypted->lines[2].value.starts_with("ENC[liquid:v1:team@1:"));

    auto decrypted = liquid::runtime::block_on(
        liquid::encryption_detail::leaf_cipher::decrypt_dotenv(*encrypted, rule, fixture.keys));
    ASSERT_TRUE(decrypted) << decrypted.error().message;
    EXPECT_EQ(*decrypted, *parsed);
}

TEST(LeafEncryption, PlaintextValidationIsUnauthorized) {
    auto parsed = liquid::formats::json::parse(R"({"token":"plaintext"})");
    ASSERT_TRUE(parsed);
    auto valid = liquid::encryption_detail::validate_json(*parsed, {"team", "token$"});
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(valid.error().context, "/token");
}

TEST(LeafEncryption, DiffNamesChangedKeysWithoutDecryption) {
    auto left = liquid::formats::json::parse(
        R"({"user":"reader","token":"ENC[liquid:v1:team:QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQQ==]"})");
    auto right = liquid::formats::json::parse(
        R"({"user":"reader","token":"ENC[liquid:v1:team:QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQg==]"})");
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    EXPECT_EQ(liquid::encryption_detail::diff_json(*left, *right),
              std::vector<std::string>{"/token"});
}
