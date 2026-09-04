#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/testing.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-repository-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    ~temporary_repository() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto content(std::string value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto sha256_options() -> liquid::repository_options {
    liquid::repository_options options;
    options.store.hash_algorithm = liquid::algorithm::sha256;
    return options;
}

auto encrypted_options(const std::shared_ptr<liquid::keyring>& keys) -> liquid::repository_options {
    liquid::repository_options options;
    options.store.keys = keys;
    return options;
}

auto encryption_keys() -> std::shared_ptr<liquid::keyring> {
    auto keys = std::make_shared<liquid::keyring>();
    EXPECT_TRUE(keys->add("repository", std::make_shared<liquid::passphrase_provider>("secret"))
                    .has_value());
    EXPECT_TRUE(keys->set_default_key_id("repository").has_value());
    return keys;
}

void expect_sha256(const liquid::digest& value) {
    EXPECT_EQ(value.alg, liquid::algorithm::sha256);
    EXPECT_TRUE(liquid::to_string(value).starts_with("sha256:"));
}

auto field(const liquid::encoding::cbor::value& value, std::string_view name)
    -> const liquid::encoding::cbor::value* {
    const auto* fields = std::get_if<liquid::encoding::cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto mutable_field(liquid::encoding::cbor::value& value, std::string_view name)
    -> liquid::encoding::cbor::value* {
    auto* fields = std::get_if<liquid::encoding::cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    EXPECT_TRUE(input.good());
    const auto size = input.tellg();
    EXPECT_GE(size, 0);
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    EXPECT_TRUE(input.good());
    return output;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

TEST(Repository, create_write_move_cut_open_and_time_travel) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("one.txt", content("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("dir/two.txt", content("two"))));
    auto before = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(before);
    ASSERT_EQ(before->entries.size(), 2U);
    const auto identity = before->entries.front().node;
    auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    auto verified = liquid::repository_detail::testing::verify_history(repository);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(*verified, 3U);
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("one.txt", "renamed.txt")));
    auto after = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(after);
    ASSERT_EQ(after->entries.front().node, identity);
    auto second = liquid::runtime::block_on(repository.cut("rename"));
    ASSERT_TRUE(second);
    auto snapshot = liquid::runtime::block_on(repository.at(*first));
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(std::ranges::any_of(snapshot->root.entries,
                                    [](const auto& entry) { return entry.name == "one.txt"; }));
    auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs);
    ASSERT_EQ(refs->size(), 1U);
    EXPECT_EQ(refs->front().id.value.bytes, second->value.bytes);

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("renamed.txt"));
    ASSERT_TRUE(value);
    EXPECT_EQ(text(*value), "one");
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_REPO"));
}

TEST(Repository, ReadabilityReportsExactBlobKeyVersionsAcrossRotation) {
    temporary_repository location;
    auto keys = encryption_keys();
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), encrypted_options(keys)));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("old.txt", content("old"))));
    const auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    const auto rotated = liquid::runtime::block_on(keys->rotate("repository"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    const auto new_source = location.path() / "new-source";
    const auto new_contents = content("new");
    write_file(new_source, new_contents);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write_file("new.txt", new_source)));
    const auto second = liquid::runtime::block_on(repository.cut("second"));
    ASSERT_TRUE(second) << second.error().message;

    const auto report = liquid::runtime::block_on(repository.readability());
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->revisions.size(), 2U);
    EXPECT_EQ(liquid::to_string(report->revisions[0].revision.value),
              liquid::to_string(second->value));
    ASSERT_EQ(report->revisions[0].keys.size(), 2U);
    EXPECT_EQ(report->revisions[0].keys[0].key_id, "repository");
    EXPECT_EQ(report->revisions[0].keys[0].version, 1U);
    EXPECT_TRUE(report->revisions[0].keys[0].readable);
    EXPECT_EQ(report->revisions[0].keys[1].key_id, "repository");
    EXPECT_EQ(report->revisions[0].keys[1].version, 2U);
    EXPECT_TRUE(report->revisions[0].keys[1].readable);
    EXPECT_EQ(liquid::to_string(report->revisions[1].revision.value),
              liquid::to_string(first->value));
    ASSERT_EQ(report->revisions[1].keys.size(), 1U);
    EXPECT_EQ(report->revisions[1].keys[0].key_id, "repository");
    EXPECT_EQ(report->revisions[1].keys[0].version, 1U);
    EXPECT_TRUE(report->revisions[1].keys[0].readable);
    ASSERT_EQ(report->limits.size(), 3U);
    EXPECT_EQ(report->limits[1],
              "Revoking a collaborator prevents future access; it does not recall what they "
              "have.");

    const auto revoked = liquid::runtime::block_on(keys->revoke("repository", 1));
    ASSERT_TRUE(revoked) << revoked.error().message;
    const auto current = liquid::runtime::block_on(repository.readability(*second));
    ASSERT_TRUE(current) << current.error().message;
    ASSERT_EQ(current->revisions.size(), 1U);
    ASSERT_EQ(current->revisions[0].keys.size(), 2U);
    EXPECT_FALSE(current->revisions[0].keys[0].readable);
    EXPECT_TRUE(current->revisions[0].keys[1].readable);
    const auto old_value = liquid::runtime::block_on(repository.read("old.txt"));
    ASSERT_FALSE(old_value);
    EXPECT_EQ(old_value.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(old_value.error().context, "repository@1");
    const auto new_value = liquid::runtime::block_on(repository.read("new.txt"));
    ASSERT_TRUE(new_value) << new_value.error().message;
    EXPECT_EQ(text(*new_value), "new");
}

TEST(Repository, write_undo_and_locks_are_recorded) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    auto operation_digest =
        liquid::hash(liquid::algorithm::blake3, root->entries.front().ref.bytes);
    ASSERT_TRUE(operation_digest);
    ASSERT_TRUE(liquid::runtime::block_on(repository.undo({*operation_digest})));
    auto missing = liquid::runtime::block_on(repository.read("file"));
    EXPECT_FALSE(missing);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("file")));
    auto refused = liquid::runtime::block_on(repository.write("file", content("replacement")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    ASSERT_TRUE(liquid::runtime::block_on(repository.unlock("file")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("replacement"))));
}

TEST(Repository, journal_replays_a_write_before_the_next_checkpoint) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("pending.txt", content("journal"))));
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_JOURNAL"));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("pending.txt"));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(text(*value), "journal");
}

TEST(Repository, Sha256StoreRoundTripsRecords) {
    temporary_repository location;
    const auto options = sha256_options();
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    auto written = liquid::runtime::block_on(repository.read("file"));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text(*written), "value");

    const auto id = liquid::runtime::block_on(repository.cut("record sha256"));
    ASSERT_TRUE(id) << id.error().message;
    expect_sha256(id->value);

    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    expect_sha256(history->front().id.value);
    expect_sha256(history->front().root);

    const auto snapshot = liquid::runtime::block_on(repository.at(history->front().id));
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    expect_sha256(snapshot->id.value);
    ASSERT_EQ(snapshot->root.entries.size(), 1U);
    expect_sha256(snapshot->root.entries.front().id);
    expect_sha256(snapshot->root.entries.front().ref);

    auto store = liquid::runtime::block_on(
        liquid::store::open(location.path() / ".liquid-store", options.store));
    ASSERT_TRUE(store) << store.error().message;
    const auto encoded_blob =
        liquid::runtime::block_on(store->get(snapshot->root.entries.front().ref));
    ASSERT_TRUE(encoded_blob) << encoded_blob.error().message;
    const auto blob = liquid::objects::decode_blob(*encoded_blob);
    ASSERT_TRUE(blob) << blob.error().message;
    ASSERT_EQ(blob->chunks.size(), 1U);
    expect_sha256(blob->chunks.front());

    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path(), options));
    ASSERT_TRUE(reopened) << reopened.error().message;
    written = liquid::runtime::block_on(reopened->read("file"));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text(*written), "value");
    const auto reopened_snapshot = liquid::runtime::block_on(reopened->at(history->front().id));
    ASSERT_TRUE(reopened_snapshot) << reopened_snapshot.error().message;
    expect_sha256(reopened_snapshot->id.value);
}

TEST(Repository, RehashesBlake3BlobsForSha256Resolution) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first blob"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("second", content("second blob"))));

    const auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_EQ(root->entries.size(), 2U);
    repository = {};

    auto store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store) << store.error().message;
    auto blobs = std::vector<std::pair<liquid::digest, std::vector<std::byte>>>{};
    for (const auto& entry : root->entries) {
        ASSERT_EQ(entry.kind, liquid::node_kind::blob);
        EXPECT_EQ(entry.ref.alg, liquid::algorithm::blake3);
        const auto blob = liquid::runtime::block_on(store->get(entry.ref));
        ASSERT_TRUE(blob) << blob.error().message;
        const auto record = liquid::objects::decode_blob(*blob);
        ASSERT_TRUE(record) << record.error().message;
        blobs.emplace_back(entry.ref, *blob);
    }
    *store = {};

    liquid::store_options sha256_store_options;
    sha256_store_options.hash_algorithm = liquid::algorithm::sha256;
    store = liquid::runtime::block_on(
        liquid::store::open(location.path() / ".liquid-store", sha256_store_options));
    ASSERT_TRUE(store) << store.error().message;
    for (const auto& [blake3_id, blob] : blobs) {
        const auto sha256_id = liquid::hash(liquid::algorithm::sha256, blob);
        ASSERT_TRUE(sha256_id) << sha256_id.error().message;
        const auto indexed = liquid::runtime::block_on(store->put(blob));
        ASSERT_TRUE(indexed) << indexed.error().message;
        EXPECT_EQ(indexed->alg, sha256_id->alg);
        EXPECT_EQ(indexed->bytes, sha256_id->bytes);

        const auto parsed_blake3 = liquid::parse_digest(liquid::to_string(blake3_id));
        const auto parsed_sha256 = liquid::parse_digest(liquid::to_string(*sha256_id));
        ASSERT_TRUE(parsed_blake3) << parsed_blake3.error().message;
        ASSERT_TRUE(parsed_sha256) << parsed_sha256.error().message;

        const auto resolved_blake3 = liquid::runtime::block_on(store->get(*parsed_blake3));
        const auto resolved_sha256 = liquid::runtime::block_on(store->get(*parsed_sha256));
        ASSERT_TRUE(resolved_blake3) << resolved_blake3.error().message;
        ASSERT_TRUE(resolved_sha256) << resolved_sha256.error().message;
        EXPECT_EQ(*resolved_blake3, blob);
        EXPECT_EQ(*resolved_sha256, blob);
    }
}

TEST(Repository, DripAndChangeSignaturesCarryEd25519Tags) {
    namespace cbor = liquid::encoding::cbor;
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("second", content("second"))));

    const auto journal = read_file(location.path() / "LIQUID_JOURNAL");
    const auto document = cbor::decode(journal);
    ASSERT_TRUE(document) << document.error().message;
    const auto* drips_value = field(*document, "drips");
    ASSERT_NE(drips_value, nullptr);
    const auto* drips = std::get_if<cbor::value::array>(&drips_value->data);
    ASSERT_NE(drips, nullptr);
    ASSERT_EQ(drips->size(), 2U);
    for (const auto& drip : *drips) {
        const auto* signature_value = field(drip, "sig");
        ASSERT_NE(signature_value, nullptr);
        const auto* signature_bytes = std::get_if<cbor::value::bytes>(&signature_value->data);
        ASSERT_NE(signature_bytes, nullptr);
        const auto signature = cbor::decode(*signature_bytes);
        ASSERT_TRUE(signature) << signature.error().message;
        const auto* algorithm_value = field(*signature, "signature_algorithm");
        ASSERT_NE(algorithm_value, nullptr);
        const auto* algorithm = std::get_if<std::string>(&algorithm_value->data);
        ASSERT_NE(algorithm, nullptr);
        EXPECT_EQ(*algorithm, "ed25519");
    }

    const auto id = liquid::runtime::block_on(repository.cut("signed change"));
    ASSERT_TRUE(id) << id.error().message;
    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    const auto signature = cbor::decode(history->front().sig);
    ASSERT_TRUE(signature) << signature.error().message;
    const auto* algorithm_value = field(*signature, "signature_algorithm");
    ASSERT_NE(algorithm_value, nullptr);
    const auto* algorithm = std::get_if<std::string>(&algorithm_value->data);
    ASSERT_NE(algorithm, nullptr);
    EXPECT_EQ(*algorithm, "ed25519");
}

TEST(Repository, RejectsAnUnknownSignatureAlgorithm) {
    namespace cbor = liquid::encoding::cbor;
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first"))));

    auto journal = cbor::decode(read_file(location.path() / "LIQUID_JOURNAL"));
    ASSERT_TRUE(journal) << journal.error().message;
    auto* drips_value = mutable_field(*journal, "drips");
    ASSERT_NE(drips_value, nullptr);
    auto* drips = std::get_if<cbor::value::array>(&drips_value->data);
    ASSERT_NE(drips, nullptr);
    ASSERT_EQ(drips->size(), 1U);
    auto* signature_value = mutable_field(drips->front(), "sig");
    ASSERT_NE(signature_value, nullptr);
    auto* signature_bytes = std::get_if<cbor::value::bytes>(&signature_value->data);
    ASSERT_NE(signature_bytes, nullptr);
    auto signature = cbor::decode(*signature_bytes);
    ASSERT_TRUE(signature) << signature.error().message;
    auto* algorithm = mutable_field(*signature, "signature_algorithm");
    ASSERT_NE(algorithm, nullptr);
    algorithm->data = std::string{"mldsa"};
    const auto encoded_signature = cbor::encode(*signature);
    ASSERT_TRUE(encoded_signature) << encoded_signature.error().message;
    signature_value->data = *encoded_signature;
    const auto encoded_journal = cbor::encode(*journal);
    ASSERT_TRUE(encoded_journal) << encoded_journal.error().message;
    auto output =
        std::ofstream{location.path() / "LIQUID_JOURNAL", std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output.good());
    output.write(reinterpret_cast<const char*>(encoded_journal->data()),
                 static_cast<std::streamsize>(encoded_journal->size()));
    ASSERT_TRUE(output.good());
    output.close();

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto verified = liquid::repository_detail::testing::verify_history(*reopened);
    ASSERT_FALSE(verified.has_value());
    EXPECT_EQ(verified.error().code, liquid::errc::unsupported);
    EXPECT_EQ(verified.error().context, "mldsa");
}

TEST(Repository, split_nest_and_combine_are_composable) {
    temporary_repository parent_path;
    temporary_repository child_path;
    temporary_repository other_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("component"))));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(parent->nest(std::move(*split), "nested")));
    auto other = liquid::runtime::block_on(liquid::repository::create(other_path.path()));
    ASSERT_TRUE(other);
    ASSERT_TRUE(liquid::runtime::block_on(other->write("other", content("other"))));
    ASSERT_TRUE(liquid::runtime::block_on(parent->combine(std::move(*other), "other-repository")));
    auto root = liquid::runtime::block_on(parent->root());
    ASSERT_TRUE(root);
    EXPECT_EQ(root->entries.size(), 3U);
}

TEST(Repository, combine_collision_is_a_conflict_value) {
    temporary_repository left_path;
    temporary_repository right_path;
    auto left = liquid::runtime::block_on(liquid::repository::create(left_path.path()));
    auto right = liquid::runtime::block_on(liquid::repository::create(right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("same", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("same", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(left->combine(std::move(*right), "same")));
    auto conflicts = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().path, "same");
    EXPECT_EQ(conflicts->front().refs.size(), 2U);
}

TEST(Repository, cut_loads_only_the_changed_directory_document) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/c/first", content("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/d/second", content("second"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("initial tree")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    liquid::repository_detail::testing::reset_directory_loads(*reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->write("a/b/c/first", content("updated"))));
    EXPECT_EQ(liquid::repository_detail::testing::directory_loads(*reopened), 1U);
}

TEST(Repository, undo_move_restores_the_original_node_identity) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("before", content("value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    const auto identity = root->entries.front().node;
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("before", "after")));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.undo(liquid::repository_detail::testing::last_operation(repository))));
    auto restored = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->entries.size(), 1U);
    EXPECT_EQ(restored->entries.front().name, "before");
    EXPECT_EQ(restored->entries.front().node, identity);
}

TEST(Repository, operations_are_newest_first_with_their_metadata) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("before", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("before", "after")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    if (!reopened) {
        ADD_FAILURE() << reopened.error().context << ": " << reopened.error().message;
        return;
    }

    auto operations = liquid::runtime::block_on(reopened->operations());
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 2U);
    EXPECT_EQ(operations->front().kind, "move");
    EXPECT_EQ(operations->front().from, "before");
    EXPECT_EQ(operations->front().to, "after");
    EXPECT_FALSE(operations->front().by.taap_id.empty());
    EXPECT_GT(operations->front().at.time_since_epoch().count(), 0);
    EXPECT_EQ(operations->back().kind, "write");
}

TEST(Repository, cross_directory_move_is_recovered_before_cut) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("from/file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("to/anchor", content("anchor"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("from/file", "to/file")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->cut("recover cross-directory move")));
    auto moved = liquid::runtime::block_on(reopened->read("to/file"));
    ASSERT_TRUE(moved);
    EXPECT_EQ(text(*moved), "value");
}

TEST(Repository, synchronized_replicas_turn_divergent_writes_into_a_conflict_value) {
    temporary_repository source_path;
    temporary_repository left_path;
    temporary_repository right_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("a.txt", content("base"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, left_path.path()));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("a.txt", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(liquid::repository_detail::testing::sync(*left, *right)));

    auto left_conflicts = liquid::runtime::block_on(left->conflicts());
    auto right_conflicts = liquid::runtime::block_on(right->conflicts());
    ASSERT_TRUE(left_conflicts);
    ASSERT_TRUE(right_conflicts);
    ASSERT_EQ(left_conflicts->size(), 1U);
    ASSERT_EQ(right_conflicts->size(), 1U);
    EXPECT_EQ(left_conflicts->front().path, "a.txt");
    EXPECT_EQ(left_conflicts->front().refs.size(), 3U);
    ASSERT_EQ(left_conflicts->front().refs.size(), right_conflicts->front().refs.size());
    for (std::size_t index = 0; index < left_conflicts->front().refs.size(); ++index) {
        EXPECT_EQ(left_conflicts->front().refs[index].alg,
                  right_conflicts->front().refs[index].alg);
        EXPECT_EQ(left_conflicts->front().refs[index].bytes,
                  right_conflicts->front().refs[index].bytes);
    }

    auto cut = liquid::runtime::block_on(left->cut("record conflict"));
    ASSERT_TRUE(cut);
    auto history = liquid::runtime::block_on(left->log());
    ASSERT_TRUE(history);
    ASSERT_FALSE(history->empty());
    EXPECT_EQ(history->front().conflicts.size(), 1U);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("resolved"))));
    ASSERT_TRUE(liquid::runtime::block_on(left->cut("resolve conflict")));
    auto resolved = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(resolved);
    EXPECT_TRUE(resolved->empty());
}

TEST(Repository, WriteFileMatchesWriteBlobAndRecordsOneDrip) {
    temporary_repository source_path;
    temporary_repository memory_path;
    temporary_repository file_path;
    auto bytes = std::vector<std::byte>(10U * 1024U * 1024U + 91U);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = std::byte{static_cast<unsigned char>((index * 37U + 11U) & 0xffU)};
    const auto input = source_path.path() / "input.bin";
    write_file(input, bytes);

    auto memory_created = liquid::runtime::block_on(liquid::repository::create(memory_path.path()));
    auto file_created = liquid::runtime::block_on(liquid::repository::create(file_path.path()));
    ASSERT_TRUE(memory_created) << memory_created.error().message;
    ASSERT_TRUE(file_created) << file_created.error().message;
    auto memory_repository = std::move(*memory_created);
    auto file_repository = std::move(*file_created);
    const auto memory_before = memory_repository.status()->pending_drips;
    const auto file_before = file_repository.status()->pending_drips;
    ASSERT_TRUE(liquid::runtime::block_on(memory_repository.write("input.bin", bytes)));
    ASSERT_TRUE(liquid::runtime::block_on(file_repository.write_file("input.bin", input)));
    EXPECT_EQ(memory_repository.status()->pending_drips, memory_before + 1);
    EXPECT_EQ(file_repository.status()->pending_drips, file_before + 1);

    const auto memory_root = liquid::runtime::block_on(memory_repository.root());
    const auto file_root = liquid::runtime::block_on(file_repository.root());
    ASSERT_TRUE(memory_root) << memory_root.error().message;
    ASSERT_TRUE(file_root) << file_root.error().message;
    ASSERT_EQ(memory_root->entries.size(), 1U);
    ASSERT_EQ(file_root->entries.size(), 1U);
    EXPECT_EQ(memory_root->entries.front().id.alg, file_root->entries.front().id.alg);
    EXPECT_EQ(memory_root->entries.front().id.bytes, file_root->entries.front().id.bytes);
    const auto read = liquid::runtime::block_on(file_repository.read("input.bin"));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, bytes);
}

} // namespace
