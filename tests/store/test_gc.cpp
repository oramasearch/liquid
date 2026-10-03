#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/store_access.hpp"
#include "write_once_backend.hpp"

namespace {

auto put_scoped(liquid::store& store, std::vector<std::byte> input)
    -> liquid::result<liquid::digest> {
    return liquid::runtime::block_on(
        liquid::repository_detail::store_access::put_plain(store, std::move(input), "fixture"));
}

class temporary_store {
public:
    temporary_store()
        : path_(liquid::test::fixture_path("liquid-gc-test-" + std::to_string(++counter_))) {
        std::filesystem::remove_all(path_);
    }
    ~temporary_store() {
        std::filesystem::remove_all(path_);
    }
    const std::filesystem::path& path() const {
        return path_;
    }

private:
    inline static unsigned counter_{};
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::size_t size, std::uint64_t seed) {
    std::vector<std::byte> output(size);
    std::mt19937_64 random(seed);
    for (auto& value : output)
        value = std::byte{static_cast<unsigned char>(random())};
    return output;
}

liquid::store open_store(const std::filesystem::path& path, std::uint64_t pack_size = 0) {
    liquid::store_options options;
    options.pack_size = pack_size;
    options.backend = liquid::sim::test_store_backend(path);
    auto opened = liquid::runtime::block_on(liquid::store::open(path, options));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

liquid::runtime::task<liquid::digest> delayed_put(liquid::store* subject,
                                                  std::vector<std::byte> input) {
    auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{2});
    if (delay_error)
        co_return std::unexpected(liquid::error{
            liquid::errc::internal, delay_error.message(), "concurrent collection delay"});
    co_return co_await subject->put(std::move(input));
}

liquid::runtime::task<liquid::digest> put_during_collection(liquid::store* store,
                                                            std::vector<std::byte> input) {
    liquid::collection_options options;
    auto [wait_error, collection, put] = co_await liquid::runtime::when_all(
        liquid::runtime::as_io(store->collect(options)),
        liquid::runtime::as_io(delayed_put(store, std::move(input))));
    if (wait_error)
        co_return std::unexpected(liquid::error{
            liquid::errc::internal, wait_error.message(), "concurrent collection test"});
    if (!collection)
        co_return std::unexpected(collection.error());
    if (!put)
        co_return std::unexpected(put.error());
    co_return *put;
}

std::uint32_t read_u32(const std::vector<std::byte>& input, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= std::uint32_t(std::to_integer<unsigned char>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::vector<std::vector<std::byte>> stored_frames(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    const std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const auto value : raw) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    std::vector<std::vector<std::byte>> frames;
    std::size_t offset = 16;
    while (offset + 49 <= bytes.size() && std::to_integer<char>(bytes[offset]) == 'L' &&
           std::to_integer<char>(bytes[offset + 1]) == 'Q' &&
           std::to_integer<char>(bytes[offset + 2]) == 'F') {
        const auto size = 49ULL + read_u32(bytes, offset + 12);
        if (size > bytes.size() - offset) {
            break;
        }
        frames.emplace_back(bytes.begin() + offset, bytes.begin() + offset + size);
        offset += size;
    }
    return frames;
}

bool contains_frame(const std::vector<std::vector<std::byte>>& haystack,
                    const std::vector<std::byte>& needle) {
    return std::ranges::any_of(haystack,
                               [&needle](const auto& candidate) { return candidate == needle; });
}

} // namespace

TEST(GarbageCollection, LiveRootsSurviveAndUnrootedChunksAreSwept) {
    temporary_store temporary;
    auto store = open_store(temporary.path(), 1);
    const auto checkout = liquid::runtime::block_on(store.put(bytes(32U * 1024U, 1)));
    const auto build = liquid::runtime::block_on(store.put(bytes(48U * 1024U, 2)));
    const auto orphan = liquid::runtime::block_on(store.put(bytes(24U * 1024U, 3)));
    ASSERT_TRUE(checkout && build && orphan);
    auto checkout_lease =
        liquid::runtime::block_on(store.lease_root(*checkout, std::chrono::hours{1}));
    auto build_lease = liquid::runtime::block_on(store.lease_root(*build, std::chrono::hours{1}));
    ASSERT_TRUE(checkout_lease && build_lease);

    liquid::collection_options options;
    options.explain = true;
    const auto report = liquid::runtime::block_on(store.collect(options));
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_EQ(report->roots, 2);
    EXPECT_EQ(report->live_objects, 2);
    EXPECT_GT(report->dead_chunks, 0);
    EXPECT_GT(report->packs_deleted, 0);
    EXPECT_FALSE(report->retained_chunks.empty());
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*checkout)).has_value());
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*build)).has_value());
    const auto removed = liquid::runtime::block_on(store.get(*orphan));
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code, liquid::errc::not_found);
}

TEST(GarbageCollection, ExpiredLeaseAgesOutAndRenewalAndReleaseAreExplicit) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto renewed_id = liquid::runtime::block_on(store.put(bytes(4096, 4)));
    ASSERT_TRUE(renewed_id.has_value());
    auto renewed =
        liquid::runtime::block_on(store.lease_root(*renewed_id, std::chrono::seconds::zero()));
    ASSERT_TRUE(renewed.has_value());
    ASSERT_TRUE(liquid::runtime::block_on(renewed->renew(std::chrono::hours{1})).has_value());
    ASSERT_TRUE(liquid::runtime::block_on(store.collect()).has_value());
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*renewed_id)).has_value());
    ASSERT_TRUE(liquid::runtime::block_on(renewed->release()).has_value());
    ASSERT_TRUE(liquid::runtime::block_on(store.collect()).has_value());
    EXPECT_EQ(liquid::runtime::block_on(store.get(*renewed_id)).error().code,
              liquid::errc::not_found);

    const auto expired_id = liquid::runtime::block_on(store.put(bytes(4096, 5)));
    ASSERT_TRUE(expired_id.has_value());
    auto expired =
        liquid::runtime::block_on(store.lease_root(*expired_id, std::chrono::seconds::zero()));
    ASSERT_TRUE(expired.has_value());
    liquid::collection_options grace;
    grace.min_lease_age = std::chrono::minutes{1};
    ASSERT_TRUE(liquid::runtime::block_on(store.collect(grace)).has_value());
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*expired_id)).has_value());
    ASSERT_TRUE(liquid::runtime::block_on(store.collect()).has_value());
    EXPECT_EQ(liquid::runtime::block_on(store.get(*expired_id)).error().code,
              liquid::errc::not_found);
}

TEST(GarbageCollection, SharedChunkSurvivesWithTheLiveObject) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    auto shared = bytes(512U * 1024U, 6);
    auto live_bytes = shared;
    auto dead_bytes = shared;
    live_bytes.insert(live_bytes.end(), 128U * 1024U, std::byte{1});
    dead_bytes.insert(dead_bytes.end(), 128U * 1024U, std::byte{2});
    const auto live = liquid::runtime::block_on(store.put(live_bytes));
    const auto dead = liquid::runtime::block_on(store.put(dead_bytes));
    ASSERT_TRUE(live && dead);
    auto root = liquid::runtime::block_on(store.lease_root(*live, std::chrono::hours{1}));
    ASSERT_TRUE(root.has_value());
    const auto report = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_GT(report->dead_chunks, 0);
    const auto got = liquid::runtime::block_on(store.get(*live));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, live_bytes);
    EXPECT_EQ(liquid::runtime::block_on(store.get(*dead)).error().code, liquid::errc::not_found);
}

TEST(GarbageCollection, TreeAndChangeRootsKeepReferencedObjects) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto payload = bytes(4096, 61);
    const auto leaf = liquid::runtime::block_on(store.put(payload));
    ASSERT_TRUE(leaf.has_value());

    liquid::tree tree;
    tree.entries.push_back({"payload", 0100644, liquid::node_kind::blob, *leaf, {}, *leaf});
    const auto tree_bytes = liquid::objects::encode(tree);
    ASSERT_TRUE(tree_bytes.has_value());
    const auto tree_id = liquid::runtime::block_on(store.put(*tree_bytes));
    ASSERT_TRUE(tree_id.has_value());

    liquid::change change{{},
                          {},
                          liquid::actor{"test"},
                          "retain tree",
                          std::chrono::sys_seconds{},
                          *tree_id,
                          {},
                          {},
                          {},
                          std::nullopt,
                          {}};
    const auto change_bytes = liquid::objects::encode(change);
    ASSERT_TRUE(change_bytes.has_value());
    const auto change_id = liquid::runtime::block_on(store.put(*change_bytes));
    ASSERT_TRUE(change_id.has_value());
    auto root = liquid::runtime::block_on(store.lease_root(*change_id, std::chrono::hours{1}));
    ASSERT_TRUE(root.has_value());

    const auto report = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(report.has_value()) << report.error().message;
    const auto retained = liquid::runtime::block_on(store.get(*leaf));
    ASSERT_TRUE(retained.has_value()) << retained.error().message;
    EXPECT_EQ(*retained, payload);
}

TEST(GarbageCollection, forever_ref_rule_survives_reopen_and_keeps_reachable_objects) {
    temporary_store temporary;
    auto options = liquid::repository_options{};
    options.retention.rules = {
        liquid::ref_retention_rule{"release tags forever", "release/*", std::nullopt}};
    auto repository =
        liquid::runtime::block_on(liquid::repository::create(temporary.path(), options));
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository->write("release.bin", bytes(4096, 72))));
    auto release = liquid::runtime::block_on(repository->cut("release"));
    ASSERT_TRUE(release) << release.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository->set_ref("release/v1", *release)));
    ASSERT_TRUE(liquid::runtime::block_on(repository->remove("release.bin")));
    ASSERT_TRUE(liquid::runtime::block_on(repository->cut("after release")));

    *repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(temporary.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto explanation = liquid::runtime::block_on(reopened->retain_explain(release->value));
    ASSERT_TRUE(explanation) << explanation.error().message;
    EXPECT_TRUE(explanation->retained);
    EXPECT_NE(explanation->reason.find("rule release tags forever"), std::string::npos);
    EXPECT_NE(explanation->reason.find("until forever"), std::string::npos);

    auto report = liquid::runtime::block_on(reopened->collect());
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->retained_by_rule["release tags forever"], 1U);
    auto snapshot = liquid::runtime::block_on(reopened->at(*release));
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto entry =
        std::ranges::find(snapshot->root.entries, "release.bin", &liquid::tree_entry::name);
    ASSERT_NE(entry, snapshot->root.entries.end());
    *reopened = {};
    auto object_store = open_store(temporary.path() / ".liquid-store");
    auto blob = liquid::runtime::block_on(object_store.get(entry->ref));
    EXPECT_TRUE(blob) << blob.error().message;
}

TEST(GarbageCollection, PutAfterEpochHighWaterSurvives) {
    temporary_store temporary;
    auto store = open_store(temporary.path(), 1);
    const auto root_id = liquid::runtime::block_on(store.put(bytes(4096, 7)));
    ASSERT_TRUE(root_id.has_value());
    auto root = liquid::runtime::block_on(store.lease_root(*root_id, std::chrono::hours{1}));
    ASSERT_TRUE(root.has_value());
    for (std::uint64_t index = 0; index != 20; ++index) {
        const auto extra = liquid::runtime::block_on(store.put(bytes(4096, 100 + index)));
        ASSERT_TRUE(extra.has_value());
    }
    const auto concurrent_bytes = bytes(96U * 1024U, 8);
    const auto concurrent =
        liquid::runtime::block_on(put_during_collection(&store, concurrent_bytes));
    ASSERT_TRUE(concurrent.has_value()) << concurrent.error().message;
    const auto got = liquid::runtime::block_on(store.get(*concurrent));
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(*got, concurrent_bytes);
    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    store = open_store(temporary.path(), 1);
    const auto rebuilt = liquid::runtime::block_on(store.get(*concurrent));
    ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error().message;
    EXPECT_EQ(*rebuilt, concurrent_bytes);
}

TEST(GarbageCollection, DryRunDoesNotChangeObjectsPacksOrEpoch) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto id = liquid::runtime::block_on(store.put(bytes(8192, 9)));
    ASSERT_TRUE(id.has_value());
    const auto pack = temporary.path() / "packs" / "pack-1.pack";
    const auto before = std::filesystem::file_size(pack);
    liquid::collection_options options;
    options.dry_run = true;
    const auto first = liquid::runtime::block_on(store.collect(options));
    const auto second = liquid::runtime::block_on(store.collect(options));
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->epoch, second->epoch);
    EXPECT_GT(first->dead_chunks, 0);
    EXPECT_EQ(before, std::filesystem::file_size(pack));
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*id)).has_value());
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "gc" / "MANIFEST"));
}

TEST(GarbageCollection, RebuiltIndexHonorsCommittedTombstones) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto live = liquid::runtime::block_on(store.put(bytes(4096, 10)));
    const auto dead = liquid::runtime::block_on(store.put(bytes(4096, 11)));
    ASSERT_TRUE(live && dead);
    auto root = liquid::runtime::block_on(store.lease_root(*live, std::chrono::hours{1}));
    ASSERT_TRUE(root.has_value());
    const auto report = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(report.has_value());
    EXPECT_GT(report->dead_chunks, 0);
    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    store = open_store(temporary.path());
    EXPECT_TRUE(liquid::runtime::block_on(store.contains(*live)).value_or(false));
    EXPECT_FALSE(liquid::runtime::block_on(store.contains(*dead)).value_or(true));
    EXPECT_TRUE(liquid::runtime::block_on(store.get(*live)).has_value());
}

TEST(GarbageCollection, DeltaBasesStayLiveWithTheirRetainedTargets) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    const auto base = bytes(32U * 1024U, 4100);
    auto target = base;
    for (auto index = std::size_t{1500}; index != 1540; ++index) {
        target[index] ^= std::byte{0x6d};
    }
    const auto base_id = put_scoped(store, base);
    const auto target_id = put_scoped(store, target);
    ASSERT_TRUE(base_id.has_value()) << base_id.error().message;
    ASSERT_TRUE(target_id.has_value()) << target_id.error().message;
    auto root = liquid::runtime::block_on(store.lease_root(*target_id, std::chrono::hours{1}));
    ASSERT_TRUE(root.has_value()) << root.error().message;
    auto options = liquid::collection_options{};
    options.compact_below = 0.0;
    const auto report = liquid::runtime::block_on(store.collect(options));
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_GE(report->live_chunks, 3U);

    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    store = open_store(temporary.path());
    const auto restored = liquid::runtime::block_on(store.get(*target_id));
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(*restored, target);
}

TEST(GarbageCollection, RepackRebasesDeltaChainsAndSurvivesReopen) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    auto contents = bytes(32U * 1024U, 4200);
    std::vector<liquid::digest> ids;
    for (auto version = std::size_t{}; version != 4; ++version) {
        if (version != 0) {
            contents[1000 + version] ^= std::byte{static_cast<unsigned char>(version)};
        }
        const auto id = put_scoped(store, contents);
        ASSERT_TRUE(id.has_value()) << id.error().message;
        ids.push_back(*id);
    }
    auto options = liquid::collection_options{};
    options.compact_only = true;
    options.repack = true;
    const auto report = liquid::runtime::block_on(store.collect(options));
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_GT(report->deltas_rebased, 0U);
    EXPECT_GT(report->packs_compacted, 0U);

    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    store = open_store(temporary.path());
    const auto restored = liquid::runtime::block_on(store.get(ids.back()));
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(*restored, contents);
}

TEST(GarbageCollection, CompactsThirtyPercentLivePackWithoutChangingFrames) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    std::vector<liquid::digest> ids;
    std::vector<liquid::lease> roots;
    for (std::uint64_t index = 0; index != 10; ++index) {
        const auto id = liquid::runtime::block_on(store.put(bytes(8192, 1000 + index)));
        ASSERT_TRUE(id.has_value());
        ids.push_back(*id);
        if (index < 3) {
            auto root = liquid::runtime::block_on(store.lease_root(*id, std::chrono::hours{1}));
            ASSERT_TRUE(root.has_value());
            roots.push_back(std::move(*root));
        }
    }
    const auto old_pack = temporary.path() / "packs" / "pack-1.pack";
    const auto old_frames = stored_frames(old_pack);
    ASSERT_EQ(old_frames.size(), 20U);

    const auto report = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_EQ(report->packs_compacted, 1U);
    EXPECT_GT(report->bytes_written_by_compaction, 0U);
    EXPECT_GT(report->bytes_reclaimed_by_compaction, 0U);
    EXPECT_FALSE(std::filesystem::exists(old_pack));
    const auto replacement = temporary.path() / "packs" / "pack-2.pack";
    const auto replacement_frames = stored_frames(replacement);
    ASSERT_EQ(replacement_frames.size(), 6U);
    for (const auto& frame : replacement_frames) {
        EXPECT_TRUE(contains_frame(old_frames, frame));
    }
    for (std::size_t index = 0; index != 3; ++index) {
        const auto got = liquid::runtime::block_on(store.get(ids[index]));
        ASSERT_TRUE(got.has_value()) << got.error().message;
        EXPECT_EQ(*got, bytes(8192, 1000 + index));
    }

    store = {};
    std::filesystem::remove_all(temporary.path() / "index");
    store = open_store(temporary.path());
    for (std::size_t index = 0; index != 3; ++index) {
        const auto got = liquid::runtime::block_on(store.get(ids[index]));
        ASSERT_TRUE(got.has_value()) << got.error().message;
        EXPECT_EQ(*got, bytes(8192, 1000 + index));
    }
}

TEST(GarbageCollection, LeavesSeventyPercentLivePackAlone) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    for (std::uint64_t index = 0; index != 10; ++index) {
        const auto id = liquid::runtime::block_on(store.put(bytes(8192, 2000 + index)));
        ASSERT_TRUE(id.has_value());
        if (index < 7) {
            auto root = liquid::runtime::block_on(store.lease_root(*id, std::chrono::hours{1}));
            ASSERT_TRUE(root.has_value());
        }
    }
    const auto report = liquid::runtime::block_on(store.collect());
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_EQ(report->packs_compacted, 0U);
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "packs" / "pack-1.pack"));
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "packs" / "pack-2.pack"));
}

TEST(GarbageCollection, DisabledCompactionChangesNothingUntilCompactOnlyPass) {
    temporary_store temporary;
    auto store = open_store(temporary.path());
    for (std::uint64_t index = 0; index != 10; ++index) {
        const auto id = liquid::runtime::block_on(store.put(bytes(8192, 3000 + index)));
        ASSERT_TRUE(id.has_value());
        if (index < 3) {
            auto root = liquid::runtime::block_on(store.lease_root(*id, std::chrono::hours{1}));
            ASSERT_TRUE(root.has_value());
        }
    }
    liquid::collection_options disabled;
    disabled.compact_below = 0.0;
    const auto swept = liquid::runtime::block_on(store.collect(disabled));
    ASSERT_TRUE(swept.has_value()) << swept.error().message;
    EXPECT_EQ(swept->packs_compacted, 0U);
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "packs" / "pack-1.pack"));

    liquid::collection_options compact_only;
    compact_only.compact_only = true;
    const auto compacted = liquid::runtime::block_on(store.collect(compact_only));
    ASSERT_TRUE(compacted.has_value()) << compacted.error().message;
    EXPECT_EQ(compacted->packs_compacted, 1U);
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "packs" / "pack-1.pack"));
}
