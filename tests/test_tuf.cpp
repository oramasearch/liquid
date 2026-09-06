#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/tuf.hpp"
#include "monocypher-ed25519.h"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;

constexpr auto verification_time =
    std::chrono::sys_days{std::chrono::year{2026} / std::chrono::September / 6};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto read_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    const auto value = std::string{std::istreambuf_iterator<char>{input}, {}};
    return bytes(value);
}

auto hex(std::span<const std::byte> value) -> std::string {
    constexpr auto alphabet = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto octet = std::to_integer<unsigned char>(byte);
        output.push_back(alphabet[octet >> 4]);
        output.push_back(alphabet[octet & 0x0f]);
    }
    return output;
}

auto sha256(std::string_view value) -> std::string {
    auto hashed = liquid::hash(liquid::algorithm::sha256, bytes(value));
    EXPECT_TRUE(hashed);
    return liquid::to_string(*hashed).substr(7);
}

struct test_key {
    json value;
    std::string id;
    std::array<std::byte, 64> secret{};

    explicit test_key(unsigned char marker) {
        auto seed = std::array<std::byte, 32>{};
        for (auto index = std::size_t{}; index < seed.size(); ++index) {
            seed[index] = static_cast<std::byte>(marker + index);
        }
        auto public_key = std::array<std::byte, 32>{};
        crypto_ed25519_key_pair(reinterpret_cast<std::uint8_t*>(secret.data()),
                                reinterpret_cast<std::uint8_t*>(public_key.data()),
                                reinterpret_cast<std::uint8_t*>(seed.data()));
        value = {{"keytype", "ed25519"},
                 {"scheme", "ed25519"},
                 {"keyval", {{"public", hex(public_key)}}}};
        id = sha256(value.dump());
    }
};

auto sign(const test_key& key, std::string_view value) -> std::string {
    auto signature = std::array<std::byte, 64>{};
    crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()),
                        reinterpret_cast<const std::uint8_t*>(key.secret.data()),
                        reinterpret_cast<const std::uint8_t*>(value.data()),
                        value.size());
    return hex(signature);
}

auto signed_document(json body, const std::vector<const test_key*>& signers) -> json {
    auto signatures = json::array();
    const auto encoded = body.dump();
    for (const auto* signer : signers) {
        signatures.push_back({{"keyid", signer->id}, {"sig", sign(*signer, encoded)}});
    }
    return {{"signed", std::move(body)}, {"signatures", std::move(signatures)}};
}

auto role(const std::vector<const test_key*>& keys, std::uint64_t threshold) -> json {
    auto ids = json::array();
    for (const auto* key : keys) {
        ids.push_back(key->id);
    }
    return {{"keyids", std::move(ids)}, {"threshold", threshold}};
}

auto root_document(std::uint64_t version,
                   const std::vector<const test_key*>& roots,
                   const std::vector<const test_key*>& targets,
                   const test_key& snapshot,
                   const test_key& timestamp,
                   const std::vector<const test_key*>& signers,
                   std::string expires = "2099-01-01T00:00:00Z") -> json {
    auto keys = json::object();
    for (const auto* key : roots) {
        keys[key->id] = key->value;
    }
    for (const auto* key : targets) {
        keys[key->id] = key->value;
    }
    keys[snapshot.id] = snapshot.value;
    keys[timestamp.id] = timestamp.value;
    auto body = json{{"_type", "root"},
                     {"spec_version", "1.0.36"},
                     {"version", version},
                     {"expires", std::move(expires)},
                     {"consistent_snapshot", true},
                     {"keys", std::move(keys)},
                     {"roles",
                      {{"root", role(roots, 2)},
                       {"targets", role(targets, 2)},
                       {"snapshot", role({&snapshot}, 1)},
                       {"timestamp", role({&timestamp}, 1)}}}};
    return signed_document(std::move(body), signers);
}

auto target_document(std::uint64_t version, const std::vector<const test_key*>& signers) -> json {
    auto body = json{{"_type", "targets"},
                     {"spec_version", "1.0.36"},
                     {"version", version},
                     {"expires", "2099-01-01T00:00:00Z"},
                     {"targets",
                      {{"attestations/entry.cbor",
                        {{"length", 4U},
                         {"hashes", {{"sha256", sha256("TAAP")}}},
                         {"custom",
                          {{"attestation_profile", "authorship/1"},
                           {"subject", "taap:1:content:sha512-256:fixture"}}}}}}}};
    return signed_document(std::move(body), signers);
}

auto binding(const json& document) -> json {
    const auto encoded = document.dump();
    return {{"version", document.at("signed").at("version")},
            {"length", encoded.size()},
            {"hashes", {{"sha256", sha256(encoded)}}}};
}

auto snapshot_document(std::uint64_t version, const json& targets, const test_key& signer) -> json {
    auto body = json{{"_type", "snapshot"},
                     {"spec_version", "1.0.36"},
                     {"version", version},
                     {"expires", "2099-01-01T00:00:00Z"},
                     {"meta", {{"targets.json", binding(targets)}}}};
    return signed_document(std::move(body), {&signer});
}

auto timestamp_document(std::uint64_t version,
                        const json& snapshot,
                        const test_key& signer,
                        std::string expires = "2099-01-01T00:00:00Z") -> json {
    auto body = json{{"_type", "timestamp"},
                     {"spec_version", "1.0.36"},
                     {"version", version},
                     {"expires", std::move(expires)},
                     {"meta", {{"snapshot.json", binding(snapshot)}}}};
    return signed_document(std::move(body), {&signer});
}

struct bundle {
    json targets;
    json snapshot;
    json timestamp;

    auto metadata(std::vector<std::vector<std::byte>> roots = {}) const
        -> liquid::tuf::update_metadata {
        return {
            .roots = std::move(roots),
            .timestamp = bytes(timestamp.dump()),
            .snapshot = bytes(snapshot.dump()),
            .targets = bytes(targets.dump()),
        };
    }
};

auto make_bundle(std::uint64_t version,
                 const std::vector<const test_key*>& targets,
                 const test_key& snapshot,
                 const test_key& timestamp,
                 std::string timestamp_expiry = "2099-01-01T00:00:00Z") -> bundle {
    auto output = bundle{};
    output.targets = target_document(version, targets);
    output.snapshot = snapshot_document(version, output.targets, snapshot);
    output.timestamp =
        timestamp_document(version, output.snapshot, timestamp, std::move(timestamp_expiry));
    return output;
}

struct keys {
    test_key root_a{1};
    test_key root_b{33};
    test_key root_c{65};
    test_key target_a{97};
    test_key target_b{129};
    test_key snapshot{161};
    test_key timestamp{193};

    auto roots() const -> std::vector<const test_key*> {
        return {&root_a, &root_b, &root_c};
    }

    auto targets() const -> std::vector<const test_key*> {
        return {&target_a, &target_b};
    }
};

class tuf_fixture {
public:
    tuf_fixture() : path_(liquid::test::fixture_path("tuf-repository-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        auto created = liquid::runtime::block_on(liquid::repository::create(path_));
        EXPECT_TRUE(created) << created.error().message;
        repository_ = std::move(*created);
    }

    ~tuf_fixture() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto fixture(std::string_view name) const -> std::vector<std::byte> {
        return read_bytes(std::filesystem::path{LIQUID_TUF_FIXTURES} / name);
    }

    auto pin() -> liquid::result<liquid::tuf::trusted_root> {
        return liquid::runtime::block_on(
            liquid::tuf::client::pin(repository_, fixture("1.root.json"), verification_time));
    }

    auto open() -> liquid::result<liquid::tuf::client> {
        return liquid::runtime::block_on(liquid::tuf::client::open(repository_, verification_time));
    }

    liquid::repository repository_;

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto fixture_bundle(const tuf_fixture& fixture) -> liquid::tuf::update_metadata {
    return {
        .timestamp = fixture.fixture("1.timestamp.json"),
        .snapshot = fixture.fixture("1.snapshot.json"),
        .targets = fixture.fixture("1.targets.json"),
    };
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        output += character == '\'' ? "'\\''" : std::string{character};
    }
    output += '\'';
    return output;
}
#endif

} // namespace

TEST(TufClient, accepts_lake_output_and_verifies_complete_target_bytes) {
    tuf_fixture fixture;
    auto pinned = fixture.pin();
    ASSERT_TRUE(pinned) << pinned.error().message;
    EXPECT_EQ(pinned->version, 1U);
    EXPECT_EQ(liquid::to_string(pinned->sha256),
              "sha256:eb04448265ea623b4fdd46fe5d96ef21141d094191ac8cdaf1a481a5c6e3773d");

    auto client = fixture.open();
    ASSERT_TRUE(client) << client.error().message;
    auto updated = liquid::runtime::block_on(client->update(fixture_bundle(fixture)));
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->versions.root, 1U);
    EXPECT_EQ(updated->versions.timestamp, 1U);
    EXPECT_EQ(updated->versions.snapshot, 1U);
    EXPECT_EQ(updated->versions.targets, 1U);
    ASSERT_EQ(updated->targets.size(), 1U);
    EXPECT_EQ(updated->targets.front().path, "attestations/entry.cbor");
    EXPECT_TRUE(client->verify_target("attestations/entry.cbor", fixture.fixture("entry.cbor")));
    EXPECT_FALSE(client->verify_target("attestations/entry.cbor", bytes("TAMP")));
    EXPECT_FALSE(client->verify_target("attestations/missing.cbor", bytes("TAAP")));
}

TEST(TufClient, counts_unique_keys_and_rejects_expired_timestamp) {
    tuf_fixture fixture;
    ASSERT_TRUE(fixture.pin());
    auto client = fixture.open();
    ASSERT_TRUE(client);
    keys trusted;

    auto duplicated = make_bundle(1, {&trusted.target_a}, trusted.snapshot, trusted.timestamp);
    duplicated.targets.at("signatures").push_back(duplicated.targets.at("signatures").front());
    duplicated.snapshot = snapshot_document(1, duplicated.targets, trusted.snapshot);
    duplicated.timestamp = timestamp_document(1, duplicated.snapshot, trusted.timestamp);
    auto threshold = liquid::runtime::block_on(client->update(duplicated.metadata()));
    ASSERT_FALSE(threshold);
    EXPECT_EQ(threshold.error().code, liquid::errc::unauthorized);
    EXPECT_NE(threshold.error().message.find("unique-key"), std::string::npos);

    auto expired = make_bundle(
        1, trusted.targets(), trusted.snapshot, trusted.timestamp, "2026-09-06T00:00:00Z");
    auto frozen = liquid::runtime::block_on(client->update(expired.metadata()));
    ASSERT_FALSE(frozen);
    EXPECT_EQ(frozen.error().context, "timestamp");
    EXPECT_NE(frozen.error().message.find("expired"), std::string::npos);
}

TEST(TufClient, persists_versions_and_rejects_rollback_and_mixed_metadata) {
    tuf_fixture fixture;
    ASSERT_TRUE(fixture.pin());
    auto client = fixture.open();
    ASSERT_TRUE(client);
    ASSERT_TRUE(liquid::runtime::block_on(client->update(fixture_bundle(fixture))));

    auto reopened = fixture.open();
    ASSERT_TRUE(reopened);
    auto rollback = liquid::runtime::block_on(reopened->update(fixture_bundle(fixture)));
    ASSERT_FALSE(rollback);
    EXPECT_EQ(rollback.error().code, liquid::errc::conflict);

    keys trusted;
    auto mixed = make_bundle(2, trusted.targets(), trusted.snapshot, trusted.timestamp);
    mixed.snapshot.at("signed").at("meta").at("targets.json").at("version") = std::uint64_t{3};
    mixed.snapshot = signed_document(mixed.snapshot.at("signed"), {&trusted.snapshot});
    mixed.timestamp = timestamp_document(2, mixed.snapshot, trusted.timestamp);
    auto mixed_result = liquid::runtime::block_on(reopened->update(mixed.metadata()));
    ASSERT_FALSE(mixed_result);
    EXPECT_NE(mixed_result.error().message.find("version does not match"), std::string::npos);

    auto changed =
        make_bundle(2, trusted.targets(), trusted.snapshot, trusted.timestamp).metadata();
    changed.targets.back() ^= std::byte{0x01};
    auto hash_mismatch = liquid::runtime::block_on(reopened->update(std::move(changed)));
    ASSERT_FALSE(hash_mismatch);
    EXPECT_NE(hash_mismatch.error().message.find("hash does not match"), std::string::npos);
}

TEST(TufClient, root_rotation_requires_both_thresholds_and_replaces_compromised_keys) {
    tuf_fixture fixture;
    ASSERT_TRUE(fixture.pin());
    keys old;
    test_key new_root_a{7};
    test_key new_root_b{39};
    test_key new_root_c{71};
    test_key new_target_a{103};
    test_key new_target_b{135};
    const auto new_roots = std::vector<const test_key*>{&new_root_a, &new_root_b, &new_root_c};
    const auto new_targets = std::vector<const test_key*>{&new_target_a, &new_target_b};

    auto old_only = root_document(
        2, new_roots, new_targets, old.snapshot, old.timestamp, {&old.root_a, &old.root_b});
    auto client = fixture.open();
    ASSERT_TRUE(client);
    auto missing_new = make_bundle(1, old.targets(), old.snapshot, old.timestamp)
                           .metadata({bytes(old_only.dump())});
    auto old_only_result = liquid::runtime::block_on(client->update(std::move(missing_new)));
    ASSERT_FALSE(old_only_result);
    EXPECT_NE(old_only_result.error().message.find("own root threshold"), std::string::npos);

    auto new_only = root_document(
        2, new_roots, new_targets, old.snapshot, old.timestamp, {&new_root_a, &new_root_b});
    auto missing_old = make_bundle(1, old.targets(), old.snapshot, old.timestamp)
                           .metadata({bytes(new_only.dump())});
    auto new_only_result = liquid::runtime::block_on(client->update(std::move(missing_old)));
    ASSERT_FALSE(new_only_result);
    EXPECT_NE(new_only_result.error().message.find("trusted root threshold"), std::string::npos);

    auto skipped = root_document(
        3, new_roots, new_targets, old.snapshot, old.timestamp, {&old.root_a, &old.root_b});
    auto skipped_result = liquid::runtime::block_on(
        client->update(make_bundle(1, old.targets(), old.snapshot, old.timestamp)
                           .metadata({bytes(skipped.dump())})));
    ASSERT_FALSE(skipped_result);
    EXPECT_NE(skipped_result.error().message.find("exactly one"), std::string::npos);

    auto both_signers =
        std::vector<const test_key*>{&old.root_a, &old.root_b, &new_root_a, &new_root_b};
    auto replacement =
        root_document(2, new_roots, new_targets, old.snapshot, old.timestamp, both_signers);
    auto compromised = make_bundle(1, old.targets(), old.snapshot, old.timestamp)
                           .metadata({bytes(replacement.dump())});
    auto refused = liquid::runtime::block_on(client->update(std::move(compromised)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().context, "targets");

    client = fixture.open();
    ASSERT_TRUE(client) << client.error().message;
    auto old_root_retry = make_bundle(1, old.targets(), old.snapshot, old.timestamp)
                              .metadata({bytes(replacement.dump())});
    auto retry_result = liquid::runtime::block_on(client->update(std::move(old_root_retry)));
    ASSERT_FALSE(retry_result);
    EXPECT_NE(retry_result.error().message.find("exactly one"), std::string::npos);

    auto accepted = make_bundle(1, new_targets, old.snapshot, old.timestamp).metadata();
    auto recovered = liquid::runtime::block_on(client->update(std::move(accepted)));
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(recovered->versions.root, 2U);
}

TEST(TufClient, root_rotation_recovers_from_an_expired_trusted_root) {
    tuf_fixture fixture;
    keys old;
    test_key new_root_a{7};
    test_key new_root_b{39};
    test_key new_root_c{71};
    test_key new_target_a{103};
    test_key new_target_b{135};
    const auto new_roots = std::vector<const test_key*>{&new_root_a, &new_root_b, &new_root_c};
    const auto new_targets = std::vector<const test_key*>{&new_target_a, &new_target_b};
    auto root_one = root_document(1,
                                  old.roots(),
                                  old.targets(),
                                  old.snapshot,
                                  old.timestamp,
                                  {&old.root_a, &old.root_b},
                                  "2026-09-07T00:00:00Z");
    auto pinned = liquid::runtime::block_on(
        liquid::tuf::client::pin(fixture.repository_, bytes(root_one.dump()), verification_time));
    ASSERT_TRUE(pinned) << pinned.error().message;

    const auto after_expiry = verification_time + std::chrono::days{2};
    auto client =
        liquid::runtime::block_on(liquid::tuf::client::open(fixture.repository_, after_expiry));
    ASSERT_TRUE(client) << client.error().message;
    const auto both_signers =
        std::vector<const test_key*>{&old.root_a, &old.root_b, &new_root_a, &new_root_b};
    auto root_two =
        root_document(2, new_roots, new_targets, old.snapshot, old.timestamp, both_signers);
    auto metadata =
        make_bundle(1, new_targets, old.snapshot, old.timestamp).metadata({bytes(root_two.dump())});
    auto recovered = liquid::runtime::block_on(client->update(std::move(metadata)));
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(recovered->versions.root, 2U);
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
TEST(TufCli, pins_and_shows_the_repository_root) {
    tuf_fixture fixture;
    const auto root = std::filesystem::path{LIQUID_TUF_FIXTURES} / "1.root.json";
    const auto output = fixture.path() / "trust.txt";
    const auto pin_command = "cd " + shell_quote(fixture.path().string()) + " && " +
                             shell_quote(LIQUID_CLI) + " registry trust pin " +
                             shell_quote(root.string()) + " > " + shell_quote(output.string());
    ASSERT_EQ(std::system(pin_command.c_str()), 0);
    const auto show_command = "cd " + shell_quote(fixture.path().string()) + " && " +
                              shell_quote(LIQUID_CLI) + " registry trust show >> " +
                              shell_quote(output.string());
    ASSERT_EQ(std::system(show_command.c_str()), 0);
    const auto shown = text(read_bytes(output));
    EXPECT_NE(shown.find("version=1"), std::string::npos);
    EXPECT_NE(shown.find("sha256=sha256:eb044482"), std::string::npos);
    EXPECT_NE(shown.find("root={\"signatures\":"), std::string::npos);
}
#endif
