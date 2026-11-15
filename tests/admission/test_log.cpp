// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../temporary_directory.hpp"
#include "admission/internal.hpp"
#include "checkpoint_fixture.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace {

using json = nlohmann::json;
namespace transparency = liquid::transparency;

auto hex(std::string_view text) -> std::vector<std::byte> {
    const auto nibble = [](char character) -> unsigned char {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned char>(character - '0');
        }
        return static_cast<unsigned char>(character - 'a' + 10);
    };
    auto output = std::vector<std::byte>{};
    output.reserve(text.size() / 2);
    for (auto index = std::size_t{}; index < text.size(); index += 2) {
        output.push_back(std::byte{
            static_cast<unsigned char>((nibble(text[index]) << 4U) | nibble(text[index + 1]))});
    }
    return output;
}

auto hash(std::string_view text) -> transparency::hash {
    const auto decoded = hex(text);
    auto output = transparency::hash{};
    std::ranges::copy(decoded, output.begin());
    return output;
}

auto base64url(std::string value) -> std::vector<std::byte> {
    for (auto& character : value) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto output = std::vector<std::byte>{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto character : value) {
        const auto position = alphabet.find(character);
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits < 8) {
            continue;
        }
        bits -= 8;
        output.push_back(std::byte{static_cast<unsigned char>((accumulator >> bits) & 0xffU)});
    }
    return output;
}

struct fixture {
    transparency::checkpoint checkpoint;
    transparency::log_trust trust;
    std::vector<transparency::tile> tiles;
    std::vector<std::vector<std::byte>> entries;
    std::vector<transparency::inclusion_proof> inclusion;
};

auto load_fixture() -> fixture {
    auto stream = std::ifstream{LIQUID_TRANSPARENCY_FIXTURE};
    const auto source = json::parse(stream);
    const auto& record = source.at("checkpoint");
    const auto& body = record.at("checkpoint");
    auto checkpoint = transparency::checkpoint{};
    checkpoint.sequence = record.at("sequence").get<std::uint64_t>();
    checkpoint.tree_size = body.at("tree_size").get<std::uint64_t>();
    checkpoint.entry_tile_width = body.at("tile_width").get<std::uint64_t>();
    checkpoint.hash_tile_width = body.at("hash_tile_width").get<std::uint64_t>();
    checkpoint.root_hash = hash(body.at("root_hash").get<std::string>());
    checkpoint.checkpoint_hash = hash(record.at("checkpoint_hash_sha256").get<std::string>());
    checkpoint.signature.signer = record.at("signature").at("signer").get<std::string>();
    checkpoint.signature.bytes =
        base64url(record.at("signature").at("signature").get<std::string>());
    const auto canonical_body = body.dump();
    checkpoint.canonical_body.assign(
        reinterpret_cast<const std::byte*>(canonical_body.data()),
        reinterpret_cast<const std::byte*>(canonical_body.data() + canonical_body.size()));

    auto configuration = transparency::log_configuration{};
    configuration.witness_threshold = 2;
    for (const auto& item : record.at("witness_signatures")) {
        const auto public_key = hash(item.at("public_key").get<std::string>());
        checkpoint.witness_signatures.push_back({item.at("endpoint").get<std::string>(),
                                                 public_key,
                                                 hex(item.at("signature").get<std::string>())});
        configuration.witnesses.push_back({item.at("endpoint").get<std::string>(), public_key});
    }
    const auto identity_key =
        base64url(source.at("lake_identity").at("public_key").get<std::string>());
    auto identity = liquid::actor{source.at("lake_identity").at("taap_id").get<std::string>(),
                                  "ed25519",
                                  {},
                                  identity_key,
                                  {},
                                  {}};

    auto tiles = std::vector<transparency::tile>{};
    for (const auto& item : source.at("tiles")) {
        auto tile = transparency::tile{};
        tile.level = item.at("level").get<std::uint32_t>();
        tile.index = item.at("index").get<std::uint64_t>();
        tile.width = item.contains("entries") ? item.at("entries").get<std::uint64_t>()
                                              : item.at("hashes").get<std::uint64_t>();
        tile.sha256 = hash(item.at("sha256").get<std::string>());
        tile.bytes = base64url(item.at("tile").get<std::string>());
        tiles.push_back(std::move(tile));
    }
    auto entries = std::vector<std::vector<std::byte>>{};
    for (const auto& item : source.at("entries")) {
        entries.push_back(base64url(item.get<std::string>()));
    }
    auto inclusion = std::vector<transparency::inclusion_proof>{};
    for (const auto& item : source.at("proofs").at("inclusion")) {
        auto proof = transparency::inclusion_proof{};
        proof.leaf_index = item.at("leaf_index").get<std::uint64_t>();
        proof.tree_size = item.at("tree_size").get<std::uint64_t>();
        proof.root_hash = hash(item.at("root_hash").get<std::string>());
        proof.bound_checkpoint = checkpoint;
        for (const auto& path_hash : item.at("audit_path")) {
            proof.audit_path.push_back(hash(path_hash.get<std::string>()));
        }
        inclusion.push_back(std::move(proof));
    }
    return {std::move(checkpoint),
            {.identity = std::move(identity), .configuration = std::move(configuration)},
            std::move(tiles),
            std::move(entries),
            std::move(inclusion)};
}

class fixture_source final : public transparency::log_source {
public:
    explicit fixture_source(fixture value) : value_(std::move(value)) {}

    std::string name() const override {
        return "admission fixture";
    }

    liquid::runtime::task<liquid::actor> identity() override {
        co_return value_.trust.identity;
    }

    liquid::runtime::task<transparency::log_configuration> configuration() override {
        co_return value_.trust.configuration;
    }

    liquid::runtime::task<transparency::checkpoint> current_checkpoint() override {
        co_return value_.checkpoint;
    }

    liquid::runtime::task<transparency::tile>
    get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) override {
        ++tile_requests;
        const auto found = std::ranges::find_if(value_.tiles, [=](const auto& tile) {
            return tile.level == level && tile.index == index && tile.width == width;
        });
        if (found == value_.tiles.end()) {
            co_return std::unexpected(
                liquid::error{liquid::errc::not_found, "requested tile is missing"});
        }
        co_return *found;
    }

    liquid::runtime::task<transparency::inclusion_proof>
    get_inclusion_proof(std::uint64_t leaf_index, std::uint64_t tree_size) override {
        const auto found = std::ranges::find_if(value_.inclusion, [=](const auto& proof) {
            return proof.leaf_index == leaf_index && proof.tree_size == tree_size;
        });
        if (found == value_.inclusion.end()) {
            co_return std::unexpected(
                liquid::error{liquid::errc::not_found, "inclusion proof is missing"});
        }
        co_return *found;
    }

    liquid::runtime::task<transparency::consistency_proof>
    get_consistency_proof(std::uint64_t old_tree_size, std::uint64_t new_tree_size) override {
        static_cast<void>(old_tree_size);
        static_cast<void>(new_tree_size);
        co_return std::unexpected(
            liquid::error{liquid::errc::not_found, "consistency proof is not configured"});
    }

    fixture value_;
    std::size_t tile_requests{};
};

auto policy_for(const fixture& value) -> liquid::admission_log_policy {
    return {.mode = liquid::admission_log_mode::verify,
            .lake = "https://lake.example",
            .trust = value.trust};
}

auto verify(fixture value,
            liquid::admission_log_policy policy,
            liquid::admission_detail::checkpoint_clock clock = std::chrono::system_clock::now)
    -> liquid::admission_log_verification {
    auto source = std::make_unique<fixture_source>(std::move(value));
    const auto entry = source->value_.entries.front();
    auto verifier = liquid::admission_detail::make_log_verifier(
        std::move(policy), std::filesystem::path{}, std::move(source), std::move(clock));
    auto result = liquid::runtime::block_on(verifier->verify_inclusion(entry));
    EXPECT_TRUE(result) << result.error().message;
    return result ? *result : liquid::admission_log_verification{};
}

TEST(AdmissionLogVerifier, accepts_a_valid_shared_transparency_proof) {
    const auto value = load_fixture();
    const auto result = verify(value, policy_for(value));
    EXPECT_EQ(result.verdict, liquid::log_verdict::included);
    ASSERT_TRUE(result.checkpoint.has_value());
    EXPECT_EQ(result.checkpoint_sequence, value.checkpoint.sequence);
    EXPECT_EQ(result.tree_size, value.checkpoint.tree_size);
}

TEST(AdmissionLogVerifier, refuses_a_forged_parent_tile_and_names_it) {
    auto value = load_fixture();
    auto policy = policy_for(value);
    value.tiles[1].bytes[17] ^= std::byte{1};
    auto changed = liquid::hash(liquid::algorithm::sha256, value.tiles[1].bytes);
    ASSERT_TRUE(changed) << changed.error().message;
    value.tiles[1].sha256 = changed->bytes;
    const auto result = verify(std::move(value), std::move(policy));
    EXPECT_EQ(result.verdict, liquid::log_verdict::not_included);
    EXPECT_NE(result.reason.find("tile level=1 index=0 width=3"), std::string::npos);
}

TEST(AdmissionLogVerifier, refuses_an_untrusted_checkpoint) {
    auto value = load_fixture();
    auto policy = policy_for(value);
    policy.trust.identity.public_key.front() ^= std::byte{1};
    const auto result = verify(std::move(value), std::move(policy));
    EXPECT_EQ(result.verdict, liquid::log_verdict::not_included);
    EXPECT_EQ(result.reason, "checkpoint log signature is invalid");
}

TEST(AdmissionLogReview, checkpoint_without_authenticated_time_fails_freshness_requirement) {
    const auto value = load_fixture();
    auto policy = policy_for(value);
    policy.max_checkpoint_age_seconds = 1;
    const auto result = verify(value, policy);
    EXPECT_NE(result.verdict, liquid::log_verdict::included)
        << "Checkpoint with no authenticated issuance time accepted under a 1-second age bound: "
        << result.reason;
    EXPECT_EQ(result.reason, "checkpoint stale");
    EXPECT_EQ(result.verdict, liquid::log_verdict::not_included);
}

auto timed_fixture(const json& issued_at) -> fixture {
    auto value = load_fixture();
    auto body = json::parse(value.checkpoint.canonical_body);
    body["issued_at"] = issued_at;
    auto signed_value = liquid::test::admission::sign_checkpoint(body);
    value.checkpoint = std::move(signed_value.checkpoint);
    value.trust = std::move(signed_value.trust);
    for (auto& proof : value.inclusion) {
        proof.bound_checkpoint = value.checkpoint;
    }
    return value;
}

auto fixed_clock(std::int64_t seconds) -> liquid::admission_detail::checkpoint_clock {
    return
        [seconds] { return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}}; };
}

TEST(AdmissionLogVerifier, enforces_authenticated_age_and_exact_skew_boundaries) {
    constexpr auto now = std::uint64_t{1700000000};
    struct example {
        std::uint64_t issued_at;
        std::uint64_t maximum_age;
        bool accepted;
    };
    for (const auto& example : {example{now, 1, true},
                                example{now - 1, 1, true},
                                example{now - 2, 1, false},
                                example{now + 60, 1, true},
                                example{now + 61, 1, false},
                                example{std::numeric_limits<std::uint64_t>::max(), 1, false},
                                example{0, std::numeric_limits<std::uint64_t>::max(), true}}) {
        SCOPED_TRACE(example.issued_at);
        auto value = timed_fixture(example.issued_at);
        auto policy = policy_for(value);
        policy.max_checkpoint_age_seconds = example.maximum_age;
        const auto result = verify(value, policy, fixed_clock(now));
        EXPECT_EQ(result.verdict,
                  example.accepted ? liquid::log_verdict::included
                                   : liquid::log_verdict::not_included);
        EXPECT_EQ(result.reason,
                  example.accepted ? "included in verified log" : "checkpoint stale");
        ASSERT_TRUE(result.checkpoint.has_value());
        EXPECT_EQ(result.checkpoint->bytes, value.checkpoint.checkpoint_hash);
        EXPECT_EQ(result.checkpoint_sequence, value.checkpoint.sequence);
        EXPECT_EQ(result.tree_size, value.checkpoint.tree_size);
    }
}

TEST(AdmissionLogVerifier, refuses_invalid_authenticated_time) {
    for (const auto& time : {json(nullptr),
                             json(-1),
                             json(1.5),
                             json("1700000000"),
                             json(true),
                             json::object(),
                             json::array()}) {
        SCOPED_TRACE(time.dump());
        auto value = timed_fixture(time);
        auto policy = policy_for(value);
        policy.max_checkpoint_age_seconds = 300;
        const auto result = verify(value, policy, fixed_clock(1700000000));
        EXPECT_EQ(result.verdict, liquid::log_verdict::not_included);
        EXPECT_EQ(result.reason, "checkpoint stale");
    }
}

TEST(AdmissionLogVerifier, omitted_bound_does_not_require_or_limit_time) {
    for (const auto& value : {load_fixture(), timed_fixture(0)}) {
        const auto result = verify(value, policy_for(value), fixed_clock(1700000000));
        EXPECT_EQ(result.verdict, liquid::log_verdict::included);
    }
}

TEST(AdmissionLogVerifier, refuses_a_time_changed_without_resigning) {
    auto value = timed_fixture(1700000000);
    auto policy = policy_for(value);
    policy.max_checkpoint_age_seconds = 1;
    auto body = json::parse(value.checkpoint.canonical_body);
    body["issued_at"] = 1700000100;
    value.checkpoint.canonical_body = liquid::test::admission::bytes(body.dump());
    value.checkpoint.checkpoint_hash =
        liquid::test::admission::sha256(value.checkpoint.canonical_body);
    const auto result = verify(value, policy, fixed_clock(1700000100));
    EXPECT_EQ(result.verdict, liquid::log_verdict::not_included);
    EXPECT_EQ(result.reason, "checkpoint log signature is invalid");
}

TEST(AdmissionLogVerifier, rechecks_replayed_checkpoints_and_preserves_the_inspection_cache) {
    constexpr auto issued_at = std::int64_t{1700000000};
    auto value = timed_fixture(issued_at);
    auto policy = policy_for(value);
    policy.max_checkpoint_age_seconds = 1;
    auto now = issued_at;
    auto source = std::make_unique<fixture_source>(value);
    auto* observed_source = source.get();
    const auto cache = liquid::test::fixture_path("admission-checkpoint-cache");
    auto verifier =
        liquid::admission_detail::make_log_verifier(policy, cache, std::move(source), [&now] {
            return std::chrono::system_clock::time_point{std::chrono::seconds{now}};
        });
    auto first = liquid::runtime::block_on(verifier->verify_inclusion(value.entries.front()));
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->verdict, liquid::log_verdict::included);
    const auto requests = observed_source->tile_requests;
    now += 2;
    auto replay = liquid::runtime::block_on(verifier->verify_inclusion(value.entries.front()));
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->verdict, liquid::log_verdict::not_included);
    EXPECT_EQ(replay->reason, "checkpoint stale");
    EXPECT_EQ(observed_source->tile_requests, requests);

    observed_source->value_ = timed_fixture(issued_at - 10);
    auto older = liquid::runtime::block_on(verifier->verify_inclusion(value.entries.front()));
    ASSERT_TRUE(older.has_value());
    EXPECT_EQ(older->reason, "checkpoint stale");
    const auto input = std::ifstream{cache, std::ios::binary};
    auto contents = std::string{std::istreambuf_iterator<char>{input.rdbuf()},
                                std::istreambuf_iterator<char>{}};
    EXPECT_EQ(contents,
              std::string(reinterpret_cast<const char*>(value.checkpoint.canonical_body.data()),
                          value.checkpoint.canonical_body.size()));
}

} // namespace
