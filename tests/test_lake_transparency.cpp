#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;
namespace transparency = liquid::transparency;

auto failure(liquid::errc code, std::string message) -> liquid::error {
    return {code, std::move(message), "transparency test source"};
}

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
    auto decoded = hex(text);
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
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

auto digest(std::span<const std::byte> bytes) -> transparency::hash {
    auto calculated = liquid::hash(liquid::algorithm::sha256, bytes);
    EXPECT_TRUE(calculated.has_value());
    return calculated->bytes;
}

auto entry_tile(std::span<const std::vector<std::byte>> entries) -> std::vector<std::byte> {
    constexpr std::string_view header = "LAKE-TILE-1\n";
    auto output = std::vector<std::byte>{};
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(header.data()),
                  reinterpret_cast<const std::byte*>(header.data() + header.size()));
    for (const auto& entry : entries) {
        const auto length = static_cast<std::uint32_t>(entry.size());
        output.push_back(std::byte{static_cast<unsigned char>(length >> 24U)});
        output.push_back(std::byte{static_cast<unsigned char>(length >> 16U)});
        output.push_back(std::byte{static_cast<unsigned char>(length >> 8U)});
        output.push_back(std::byte{static_cast<unsigned char>(length)});
        output.insert(output.end(), entry.begin(), entry.end());
    }
    return output;
}

struct fixture {
    transparency::checkpoint checkpoint;
    liquid::actor identity;
    transparency::log_configuration configuration;
    std::vector<transparency::tile> tiles;
    std::vector<std::vector<std::byte>> entries;
    std::vector<transparency::inclusion_proof> inclusion;
    transparency::consistency_proof consistency;
    transparency::checkpoint old_checkpoint;
};

auto load_fixture() -> fixture {
    auto stream = std::ifstream{LIQUID_TRANSPARENCY_FIXTURE};
    auto source = json{};
    stream >> source;
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
    const auto dumped = body.dump();
    checkpoint.canonical_body.assign(
        reinterpret_cast<const std::byte*>(dumped.data()),
        reinterpret_cast<const std::byte*>(dumped.data() + dumped.size()));
    auto configuration = transparency::log_configuration{};
    configuration.witness_threshold = 2;
    for (const auto& item : record.at("witness_signatures")) {
        auto public_key = hash(item.at("public_key").get<std::string>());
        checkpoint.witness_signatures.push_back({item.at("endpoint").get<std::string>(),
                                                 public_key,
                                                 hex(item.at("signature").get<std::string>())});
        configuration.witnesses.push_back({item.at("endpoint").get<std::string>(), public_key});
    }
    const auto key = base64url(source.at("lake_identity").at("public_key").get<std::string>());
    auto identity = liquid::actor{
        source.at("lake_identity").at("taap_id").get<std::string>(), "ed25519", {}, key, {}};
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
        for (const auto& item_hash : item.at("audit_path")) {
            proof.audit_path.push_back(hash(item_hash.get<std::string>()));
        }
        inclusion.push_back(std::move(proof));
    }
    const auto& consistency_json = source.at("proofs").at("consistency");
    auto consistency = transparency::consistency_proof{};
    consistency.old_tree_size = consistency_json.at("old_tree_size").get<std::uint64_t>();
    consistency.new_tree_size = consistency_json.at("new_tree_size").get<std::uint64_t>();
    consistency.old_root_hash = hash(consistency_json.at("old_root_hash").get<std::string>());
    consistency.new_root_hash = hash(consistency_json.at("new_root_hash").get<std::string>());
    consistency.bound_checkpoint = checkpoint;
    for (const auto& item_hash : consistency_json.at("audit_path")) {
        consistency.audit_path.push_back(hash(item_hash.get<std::string>()));
    }
    auto old_checkpoint = transparency::checkpoint{};
    old_checkpoint.tree_size = consistency.old_tree_size;
    old_checkpoint.root_hash = consistency.old_root_hash;
    return {std::move(checkpoint),
            std::move(identity),
            std::move(configuration),
            std::move(tiles),
            std::move(entries),
            std::move(inclusion),
            std::move(consistency),
            std::move(old_checkpoint)};
}

class fixture_source final : public transparency::log_source {
public:
    explicit fixture_source(fixture data) : data_(std::move(data)) {}

    [[nodiscard]] std::string name() const override {
        return "fixture log";
    }

    [[nodiscard]] liquid::runtime::task<liquid::actor> identity() override {
        co_return data_.identity;
    }

    [[nodiscard]] liquid::runtime::task<transparency::log_configuration> configuration() override {
        co_return data_.configuration;
    }

    [[nodiscard]] liquid::runtime::task<transparency::checkpoint> current_checkpoint() override {
        co_return data_.checkpoint;
    }

    [[nodiscard]] liquid::runtime::task<transparency::tile>
    get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) override {
        ++tile_reads;
        if (missing_tile || missing_level == level) {
            co_return std::unexpected(
                failure(liquid::errc::not_found, "requested tile is missing"));
        }
        const auto found = std::ranges::find_if(data_.tiles, [=](const auto& tile) {
            return tile.level == level && tile.index == index && tile.width == width;
        });
        if (found == data_.tiles.end()) {
            co_return std::unexpected(
                failure(liquid::errc::not_found, "requested tile is missing"));
        }
        co_return *found;
    }

    [[nodiscard]] liquid::runtime::task<transparency::inclusion_proof>
    get_inclusion_proof(std::uint64_t, std::uint64_t) override {
        co_return std::unexpected(
            failure(liquid::errc::not_found, "inclusion proof is not configured"));
    }

    [[nodiscard]] liquid::runtime::task<transparency::consistency_proof>
    get_consistency_proof(std::uint64_t, std::uint64_t) override {
        ++consistency_requests;
        co_return data_.consistency;
    }

    fixture data_;
    bool missing_tile{};
    std::optional<std::uint32_t> missing_level;
    std::size_t tile_reads{};
    std::size_t consistency_requests{};
};

TEST(transparency, verifies_lake_conformance_fixture) {
    auto data = load_fixture();
    ASSERT_TRUE(data.checkpoint.verify({data.identity, data.configuration}));
    for (auto index = std::size_t{}; index < data.entries.size(); ++index) {
        EXPECT_TRUE(data.inclusion[index].verify(data.entries[index], data.checkpoint));
    }
    EXPECT_TRUE(data.consistency.verify(data.old_checkpoint, data.checkpoint));
    auto source = fixture_source{std::move(data)};
    auto client = transparency::log_client{source};
    EXPECT_TRUE(liquid::runtime::block_on(client.verify_tiles(source.data_.checkpoint)));
}

TEST(transparency, refuses_modified_missing_reordered_and_substituted_tiles) {
    auto data = load_fixture();
    auto modified = fixture_source{data};
    modified.data_.tiles[0].bytes[16] ^= std::byte{1};
    EXPECT_FALSE(modified.data_.tiles[0].entries());

    auto missing = fixture_source{data};
    missing.missing_tile = true;
    auto missing_client = transparency::log_client{missing};
    EXPECT_FALSE(liquid::runtime::block_on(missing_client.verify_tiles(missing.data_.checkpoint)));

    auto reordered = fixture_source{data};
    std::swap(reordered.data_.entries[0], reordered.data_.entries[1]);
    reordered.data_.tiles[0].bytes = entry_tile(reordered.data_.entries);
    reordered.data_.tiles[0].sha256 = digest(reordered.data_.tiles[0].bytes);
    auto reordered_client = transparency::log_client{reordered};
    EXPECT_FALSE(
        liquid::runtime::block_on(reordered_client.verify_tiles(reordered.data_.checkpoint)));

    auto substituted = fixture_source{std::move(data)};
    substituted.data_.tiles[0].bytes[16] ^= std::byte{1};
    substituted.data_.tiles[0].sha256 = digest(substituted.data_.tiles[0].bytes);
    auto substituted_client = transparency::log_client{substituted};
    EXPECT_FALSE(
        liquid::runtime::block_on(substituted_client.verify_tiles(substituted.data_.checkpoint)));
}

TEST(transparency, cve_2026_56865_refuses_a_valid_leaf_tile_with_wrong_parent) {
    auto data = load_fixture();
    auto parent = fixture_source{data};
    parent.data_.tiles[1].bytes[17] ^= std::byte{1};
    parent.data_.tiles[1].sha256 = digest(parent.data_.tiles[1].bytes);
    auto parent_client = transparency::log_client{parent};
    auto verified = liquid::runtime::block_on(parent_client.verify_tiles(parent.data_.checkpoint));
    ASSERT_FALSE(verified);
    EXPECT_NE(verified.error().message.find("tile level=1 index=0 width=3"), std::string::npos);

    data.inclusion[0].audit_path[0][0] ^= std::byte{1};
    EXPECT_FALSE(data.inclusion[0].verify(data.entries[0], data.checkpoint));
    data.consistency.audit_path[0][0] ^= std::byte{1};
    EXPECT_FALSE(data.consistency.verify(data.old_checkpoint, data.checkpoint));
}

TEST(transparency, cached_tiles_are_reauthenticated_to_the_selected_checkpoint) {
    auto source = fixture_source{load_fixture()};
    auto client = transparency::log_client{source};
    ASSERT_TRUE(liquid::runtime::block_on(client.verify_tiles(source.data_.checkpoint)));
    const auto cold_reads = source.tile_reads;
    EXPECT_EQ(cold_reads, source.data_.tiles.size());

    auto forged_checkpoint = source.data_.checkpoint;
    forged_checkpoint.root_hash[0] ^= std::byte{1};
    auto verified = liquid::runtime::block_on(client.verify_tiles(std::move(forged_checkpoint)));
    ASSERT_FALSE(verified);
    EXPECT_EQ(source.tile_reads, cold_reads);
    EXPECT_EQ(verified.error().message, "entry and parent tiles do not reach checkpoint root");
}

TEST(transparency, consistency_refuses_a_skipped_tile_level_before_evaluating_the_proof) {
    auto source = fixture_source{load_fixture()};
    source.missing_level = 1;
    auto repository = liquid::runtime::block_on(
        liquid::repository::create(liquid::test::fixture_path("transparency-consistency")));
    ASSERT_TRUE(repository) << repository.error().message;
    auto trusted = liquid::runtime::block_on(
        transparency::trust_checkpoint(*repository,
                                       source.name(),
                                       source.data_.old_checkpoint,
                                       transparency::bootstrap_policy::pinned));
    ASSERT_TRUE(trusted) << trusted.error().message;

    auto client = transparency::log_client{source};
    auto verified = liquid::runtime::block_on(client.verify_consistency(*repository));
    ASSERT_FALSE(verified);
    EXPECT_NE(verified.error().message.find("tile level=1 index=0 width=3"), std::string::npos);
    EXPECT_EQ(source.consistency_requests, 0U);

    source.missing_level.reset();
    auto accepted = liquid::runtime::block_on(client.verify_consistency(*repository));
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_EQ(source.consistency_requests, 1U);
}

TEST(transparency, requires_distinct_configured_witnesses) {
    auto data = load_fixture();
    data.configuration.witness_threshold = 3;
    auto verified = data.checkpoint.verify({data.identity, data.configuration});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().message, "checkpoint witness threshold not met");
}

} // namespace
