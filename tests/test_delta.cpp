#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "build/remote_blob_source.hpp"
#include "liquid/attestation.hpp"
#include "liquid/delta.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

class delta_fixture : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::current_path() / ("delta-test-" + std::to_string(++counter_));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        if (const auto* configured = std::getenv("LIQUID_HOME")) {
            prior_liquid_home_ = configured;
        }
#ifdef _WIN32
        ASSERT_EQ(_putenv_s("LIQUID_HOME", (root_ / "identity").string().c_str()), 0);
#else
        ASSERT_EQ(setenv("LIQUID_HOME", (root_ / "identity").string().c_str(), 1), 0);
#endif
    }

    void TearDown() override {
#ifdef _WIN32
        EXPECT_EQ(_putenv_s("LIQUID_HOME", prior_liquid_home_.value_or("").c_str()), 0);
#else
        if (prior_liquid_home_) {
            EXPECT_EQ(setenv("LIQUID_HOME", prior_liquid_home_->c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv("LIQUID_HOME"), 0);
        }
#endif
        std::filesystem::remove_all(root_);
    }

    auto open(std::string_view name) -> liquid::store {
        auto opened = liquid::runtime::block_on(liquid::store::open(root_ / name));
        EXPECT_TRUE(opened) << (opened ? "" : opened.error().message);
        return std::move(*opened);
    }

    auto open_with_remote(std::string_view name,
                          std::shared_ptr<liquid::test_support::on_disk_remote_source> remote)
        -> liquid::store {
        auto options = liquid::store_options{};
        options.remote_sources.emplace("disk", std::move(remote));
        auto opened =
            liquid::runtime::block_on(liquid::store::open(root_ / name, std::move(options)));
        EXPECT_TRUE(opened) << (opened ? "" : opened.error().message);
        return std::move(*opened);
    }

    auto write(std::string_view name, std::string_view value) -> std::filesystem::path {
        const auto path = root_ / name;
        auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
        EXPECT_TRUE(output);
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        EXPECT_TRUE(output);
        return path;
    }

    auto read(const std::filesystem::path& path) -> std::string {
        auto input = std::ifstream{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    static auto content(std::string_view changed) -> std::string {
        auto output = std::string{};
        for (auto index = 0; index != 4096; ++index) {
            output += "artifact line ";
            output += std::to_string(index);
            output += index == 2048 ? changed : " stable";
            output += '\n';
        }
        return output;
    }

    inline static unsigned counter_{};
    std::filesystem::path root_;
    std::optional<std::string> prior_liquid_home_;
};

auto has_attribute(const liquid::attestation& value,
                   std::string_view name,
                   std::string_view expected) -> bool {
    const auto found =
        std::ranges::find(value.attributes, name, &liquid::attestation_attribute::name);
    if (found == value.attributes.end()) {
        return false;
    }
    const auto* text = std::get_if<std::string>(&found->value);
    return text && *text == expected;
}

TEST_F(delta_fixture, CreatesAppliesAndValidatesAttestation) {
    const auto source_text = content(" before");
    const auto target_text = content(" after");
    const auto source = write("source.txt", source_text);
    const auto target = write("target.txt", target_text);
    const auto output = root_ / "output.txt";
    auto store = open("store");

    auto created = liquid::runtime::block_on(liquid::create_delta(store, source, target));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_LT(created->delta_bytes, created->target_bytes);

    auto record_bytes = liquid::runtime::block_on(store.get(created->record));
    ASSERT_TRUE(record_bytes) << record_bytes.error().message;
    auto record = liquid::decode_delta(*record_bytes);
    ASSERT_TRUE(record) << record.error().message;
    EXPECT_EQ(liquid::to_string(record->source), liquid::to_string(created->value.source));
    EXPECT_EQ(liquid::to_string(record->target), liquid::to_string(created->value.target));
    EXPECT_EQ(record->algorithm, liquid::delta_algorithm::zstd_patch);

    const auto statements = store.attestations(created->value.target);
    ASSERT_NE(std::ranges::find(statements,
                                liquid::to_string(created->attestation),
                                [](const auto& value) { return liquid::to_string(value); }),
              statements.end());
    auto statement_bytes = liquid::runtime::block_on(store.get(created->attestation));
    ASSERT_TRUE(statement_bytes) << statement_bytes.error().message;
    auto statement = liquid::decode_attestation(*statement_bytes);
    ASSERT_TRUE(statement) << statement.error().message;
    EXPECT_EQ(statement->profile, "delta/1");
    EXPECT_TRUE(has_attribute(*statement, "source", liquid::to_string(created->value.source)));
    EXPECT_TRUE(has_attribute(*statement, "target", liquid::to_string(created->value.target)));
    EXPECT_TRUE(has_attribute(*statement, "algorithm", "zstd-patch"));
    EXPECT_TRUE(has_attribute(*statement, "delta", liquid::to_string(created->record)));
    auto registered = liquid::attestation_profile_registry::global().validate(*statement);
    ASSERT_TRUE(registered) << registered.error().message;
    auto verified = liquid::verify_attestation(*statement);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;

    auto applied =
        liquid::runtime::block_on(liquid::apply_delta(store, created->record, source, output));
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(read(output), target_text);
}

TEST_F(delta_fixture, FetchUsesLocalDelta) {
    const auto source = write("source.txt", content(" old"));
    const auto target = write("target.txt", content(" new"));
    const auto output = root_ / "output.txt";
    auto store = open("store");
    auto created = liquid::runtime::block_on(liquid::create_delta(store, source, target));
    ASSERT_TRUE(created) << created.error().message;

    auto fetched = liquid::runtime::block_on(
        liquid::fetch_delta(store, created->value.target, source, output));
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->method, liquid::delta_fetch_method::delta);
    ASSERT_TRUE(fetched->record);
    EXPECT_EQ(liquid::to_string(*fetched->record), liquid::to_string(created->record));
    EXPECT_EQ(read(output), read(target));
}

TEST_F(delta_fixture, FetchFallsBackToChunksWhenNoDeltaExists) {
    const auto source = write("source.txt", content(" old"));
    const auto target = write("target.txt", content(" new"));
    const auto output = root_ / "output.txt";
    auto store = open("store");
    auto target_id = liquid::runtime::block_on(store.put_file(target));
    ASSERT_TRUE(target_id) << target_id.error().message;

    auto fetched =
        liquid::runtime::block_on(liquid::fetch_delta(store, *target_id, source, output));
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->method, liquid::delta_fetch_method::chunks);
    EXPECT_FALSE(fetched->record);
    EXPECT_EQ(read(output), read(target));
}

TEST_F(delta_fixture, FetchUsesDeltaFromRemoteStoreDouble) {
    const auto source = write("source.txt", content(" old"));
    const auto target = write("target.txt", content(" new"));
    const auto output = root_ / "output.txt";
    auto catalog = open("catalog");
    auto created = liquid::runtime::block_on(liquid::create_delta(catalog, source, target));
    ASSERT_TRUE(created) << created.error().message;
    auto record = liquid::runtime::block_on(catalog.get(created->record));
    ASSERT_TRUE(record) << record.error().message;
    auto attestation = liquid::runtime::block_on(catalog.get(created->attestation));
    ASSERT_TRUE(attestation) << attestation.error().message;
    auto patch = liquid::runtime::block_on(catalog.get(created->value.blob));
    ASSERT_TRUE(patch) << patch.error().message;
    auto remote_store = open("remote");
    auto remote =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(remote_store));
    auto remote_patch = remote->put(std::move(*patch));
    ASSERT_TRUE(remote_patch) << remote_patch.error().message;
    remote_patch->remote = liquid::remote_blob_location{"disk", "worker-a"};
    auto local = open_with_remote("local", remote);
    auto stored_record = liquid::runtime::block_on(local.put(std::move(*record)));
    ASSERT_TRUE(stored_record) << stored_record.error().message;
    EXPECT_EQ(liquid::to_string(*stored_record), liquid::to_string(created->record));
    auto stored_attestation = liquid::runtime::block_on(local.put(std::move(*attestation)));
    ASSERT_TRUE(stored_attestation) << stored_attestation.error().message;
    EXPECT_EQ(liquid::to_string(*stored_attestation), liquid::to_string(created->attestation));
    auto registered = liquid::runtime::block_on(local.register_remote(*remote_patch));
    ASSERT_TRUE(registered) << registered.error().message;

    auto fetched = liquid::runtime::block_on(
        liquid::fetch_delta(local, created->value.target, source, output));
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->method, liquid::delta_fetch_method::delta);
    ASSERT_EQ(remote->read_blobs().size(), 1U);
    EXPECT_EQ(liquid::to_string(remote->read_blobs().front()),
              liquid::to_string(created->value.blob));
    EXPECT_EQ(read(output), read(target));
}

TEST_F(delta_fixture, RemoteFetchFallsBackToChunksWhenNoDeltaExists) {
    const auto source = write("source.txt", content(" old"));
    const auto target = write("target.txt", content(" new"));
    const auto output = root_ / "output.txt";
    auto remote_store = open("remote");
    auto remote =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(remote_store));
    const auto target_text = read(target);
    auto target_bytes = std::vector<std::byte>(std::as_bytes(std::span{target_text}).begin(),
                                               std::as_bytes(std::span{target_text}).end());
    auto remote_target = remote->put(std::move(target_bytes));
    ASSERT_TRUE(remote_target) << remote_target.error().message;
    remote_target->remote = liquid::remote_blob_location{"disk", "worker-a"};
    auto local = open_with_remote("local", remote);
    auto registered = liquid::runtime::block_on(local.register_remote(*remote_target));
    ASSERT_TRUE(registered) << registered.error().message;

    auto fetched = liquid::runtime::block_on(
        liquid::fetch_delta(local, remote_target->content, source, output));
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->method, liquid::delta_fetch_method::chunks);
    ASSERT_EQ(remote->read_blobs().size(), 1U);
    EXPECT_EQ(liquid::to_string(remote->read_blobs().front()),
              liquid::to_string(remote_target->content));
    EXPECT_EQ(read(output), read(target));
}

TEST_F(delta_fixture, RejectsDeltaThatProducesWrongTargetDigest) {
    const auto source_text = content(" old");
    const auto first_target_text = content(" first");
    const auto second_target_text = content(" second");
    auto source_id = liquid::hash(liquid::algorithm::blake3, std::as_bytes(std::span{source_text}));
    auto first_target_id =
        liquid::hash(liquid::algorithm::blake3, std::as_bytes(std::span{first_target_text}));
    ASSERT_TRUE(source_id);
    ASSERT_TRUE(first_target_id);
    auto wrong_patch = liquid::make_delta(liquid::delta_algorithm::zstd_patch,
                                          std::as_bytes(std::span{source_text}),
                                          std::as_bytes(std::span{second_target_text}));
    ASSERT_TRUE(wrong_patch) << wrong_patch.error().message;
    auto patch_id = liquid::hash(liquid::algorithm::blake3, *wrong_patch);
    ASSERT_TRUE(patch_id);
    auto record = liquid::delta_record{
        *source_id, *first_target_id, liquid::delta_algorithm::zstd_patch, *patch_id};

    auto applied = liquid::apply_delta(record, std::as_bytes(std::span{source_text}), *wrong_patch);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, liquid::errc::corrupt);
    EXPECT_EQ(applied.error().message, "applied delta does not match target digest");
}

TEST_F(delta_fixture, RejectsCorruptedDeltaBlobDigest) {
    const auto source_text = content(" old");
    const auto target_text = content(" new");
    auto source_id = liquid::hash(liquid::algorithm::blake3, std::as_bytes(std::span{source_text}));
    auto target_id = liquid::hash(liquid::algorithm::blake3, std::as_bytes(std::span{target_text}));
    ASSERT_TRUE(source_id);
    ASSERT_TRUE(target_id);
    auto patch = liquid::make_delta(liquid::delta_algorithm::zstd_patch,
                                    std::as_bytes(std::span{source_text}),
                                    std::as_bytes(std::span{target_text}));
    ASSERT_TRUE(patch) << patch.error().message;
    auto patch_id = liquid::hash(liquid::algorithm::blake3, *patch);
    ASSERT_TRUE(patch_id);
    auto record = liquid::delta_record{
        *source_id, *target_id, liquid::delta_algorithm::zstd_patch, *patch_id};
    patch->front() ^= std::byte{1};

    auto applied = liquid::apply_delta(record, std::as_bytes(std::span{source_text}), *patch);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, liquid::errc::corrupt);
    EXPECT_EQ(applied.error().message, "delta blob does not match record digest");
}

} // namespace
