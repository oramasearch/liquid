#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"

namespace {

class redaction_repository {
public:
    redaction_repository()
        : path_(liquid::test::fixture_path("liquid-redaction-test-" +
                                           std::to_string(next_.fetch_add(1)))) {
        std::filesystem::remove_all(path_);
    }

    ~redaction_repository() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic<std::uint64_t> next_{};
    std::filesystem::path path_;
};

auto content(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

TEST(Redaction, replacement_survives_and_prior_objects_are_unreadable) {
    redaction_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto sensitive = std::string{"AKIA"} + "0123456789ABCDEF";
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("settings.env", content("key=" + sensitive + "\n"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_EQ(root->entries.size(), 1U);
    const auto old_blob = root->entries.front().ref;
    auto before_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(before_store) << before_store.error().message;
    auto encoded_blob = liquid::runtime::block_on(before_store->get(old_blob));
    ASSERT_TRUE(encoded_blob) << encoded_blob.error().message;
    auto blob = liquid::objects::decode_blob(*encoded_blob);
    ASSERT_TRUE(blob) << blob.error().message;
    ASSERT_FALSE(blob->chunks.empty());
    const auto old_content = blob->chunks.front();
    before_store = {};

    auto redacted =
        liquid::runtime::block_on(repository.redact("settings.env", content("key=[redacted]\n")));
    ASSERT_TRUE(redacted) << redacted.error().message;
    auto replacement = liquid::runtime::block_on(repository.read("settings.env"));
    ASSERT_TRUE(replacement) << replacement.error().message;
    EXPECT_EQ(text(*replacement), "key=[redacted]\n");
    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    replacement = liquid::runtime::block_on(reopened->read("settings.env"));
    ASSERT_TRUE(replacement) << replacement.error().message;
    EXPECT_EQ(text(*replacement), "key=[redacted]\n");
    *reopened = liquid::repository{};

    auto store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store) << store.error().message;
    auto missing_blob = liquid::runtime::block_on(store->get(old_blob));
    ASSERT_FALSE(missing_blob);
    EXPECT_EQ(missing_blob.error().code, liquid::errc::not_found);
    auto missing_content = liquid::runtime::block_on(store->get(old_content));
    ASSERT_FALSE(missing_content);
    EXPECT_EQ(missing_content.error().code, liquid::errc::not_found);
}

TEST(Redaction, current_content_can_replace_a_named_prior_value) {
    redaction_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("settings.env", content("token-value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    const auto old_blob = root->entries.front().ref;
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("settings.env", content("[redacted]"))));
    auto redacted = liquid::runtime::block_on(repository.redact("settings.env"));
    ASSERT_TRUE(redacted) << redacted.error().message;
    auto replacement = liquid::runtime::block_on(repository.read("settings.env"));
    ASSERT_TRUE(replacement) << replacement.error().message;
    EXPECT_EQ(text(*replacement), "[redacted]");
    auto store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store) << store.error().message;
    auto missing = liquid::runtime::block_on(store->get(old_blob));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);
}

} // namespace
