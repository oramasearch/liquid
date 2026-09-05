#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "../temporary_directory.hpp"
#include "daemon/fanout.hpp"
#include "liquid/daemon.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "search/index.hpp"

namespace {

static_assert(std::is_aggregate_v<liquid::search_options>);
static_assert(std::is_aggregate_v<liquid::search_hit>);
static_assert(std::is_aggregate_v<liquid::search_plan>);

class temporary_directory {
public:
    temporary_directory()
        : path_(liquid::test::fixture_path("liquid-search-test-" +
                                           std::to_string(next_.fetch_add(1)))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic<std::uint64_t> next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto format_fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_FORMAT_FIXTURES} / name;
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("format fixture is absent: " + path.string());
    }
    const auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

auto document(std::string path, std::string contents) -> liquid::search_detail::document {
    auto raw = bytes(contents);
    auto id = liquid::hash(liquid::algorithm::blake3, raw);
    if (!id) {
        throw std::runtime_error(id.error().message);
    }
    return {std::move(path), *id, std::move(raw)};
}

auto fingerprint(std::string_view value) -> liquid::digest {
    auto id = liquid::hash(liquid::algorithm::blake3, bytes(value));
    if (!id) {
        throw std::runtime_error(id.error().message);
    }
    return *id;
}

auto blob_file_count(const std::filesystem::path& index) -> std::uint64_t {
    auto count = std::uint64_t{};
    const auto directory = index / "blobs";
    if (!std::filesystem::exists(directory)) {
        return count;
    }
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

auto wait_for_search_delivery(liquid::daemon& daemon, std::uint64_t before)
    -> liquid::result<liquid::daemon_status> {
    for (auto attempt = 0; attempt != 500; ++attempt) {
        auto status = liquid::runtime::block_on(daemon.status());
        if (!status) {
            return std::unexpected(status.error());
        }
        const auto search =
            std::ranges::find(status->targets, "search", &liquid::daemon_target_status::name);
        if (search != status->targets.end() && search->deliveries > before && search->behind == 0) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return std::unexpected(liquid::error{
        liquid::errc::unavailable, "search target did not receive drip", "search test"});
}

auto hit_identity(const liquid::search_hit& hit) {
    return std::tuple{
        hit.path, liquid::to_string(hit.blob), hit.line, hit.column, hit.snippet, hit.score};
}

} // namespace

TEST(SearchIndex, repository_search_indexes_shared_blob_once) {
    temporary_directory location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << (created ? "" : created.error().message);
    auto repository = std::move(*created);
    const auto contents = bytes("a content addressed phrase appears once");
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("one.txt", contents)));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("nested/two.txt", contents)));

    auto hits = liquid::runtime::block_on(repository.search("addressed phrase"));
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 2);
    EXPECT_EQ(blob_file_count(location.path() / ".liquid" / "index"), 1);
    EXPECT_EQ(liquid::to_string((*hits)[0].blob), liquid::to_string((*hits)[1].blob));

    auto path_hit = liquid::runtime::block_on(repository.search("nested"));
    ASSERT_TRUE(path_hit) << (path_hit ? "" : path_hit.error().message);
    ASSERT_EQ(path_hit->size(), 1);
    EXPECT_EQ(path_hit->front().path, "nested/two.txt");
}

TEST(SearchIndex, repositorySearchFindsDecompressedArchiveEntries) {
    temporary_directory location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << (created ? "" : created.error().message);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("archive.zip", format_fixture("archive-a.zip"))));

    auto hits = liquid::runtime::block_on(repository.search("original archive value"));
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    const auto found =
        std::ranges::find(*hits, "archive.zip!/deflated.txt", &liquid::search_hit::path);
    ASSERT_NE(found, hits->end());
    auto expected = liquid::hash(liquid::algorithm::blake3, bytes("original archive value\n"));
    ASSERT_TRUE(expected) << expected.error().message;
    EXPECT_EQ(liquid::to_string(found->blob), liquid::to_string(*expected));
}

TEST(SearchIndex, repository_search_selects_named_reference) {
    temporary_directory location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << (created ? "" : created.error().message);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("version.txt", bytes("historical search marker"))));
    auto historical = liquid::runtime::block_on(repository.cut("historical"));
    ASSERT_TRUE(historical) << (historical ? "" : historical.error().message);
    ASSERT_TRUE(liquid::runtime::block_on(repository.set_ref("saved", *historical)));
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("version.txt", bytes("current marker"))));

    auto saved = liquid::runtime::block_on(
        repository.search("historical", liquid::search_options{.ref = "saved"}));
    ASSERT_TRUE(saved) << (saved ? "" : saved.error().message);
    ASSERT_EQ(saved->size(), 1);
    EXPECT_EQ(saved->front().path, "version.txt");

    auto current = liquid::runtime::block_on(repository.search("historical"));
    ASSERT_TRUE(current) << (current ? "" : current.error().message);
    EXPECT_TRUE(current->empty());
}

TEST(SearchIndex, positional_trigrams_match_only_contiguous_substrings) {
    temporary_directory location;
    const auto index = location.path() / "index";
    const auto documents = std::vector{document("match.txt", "alpha needle omega"),
                                       document("miss.txt", "needle alpha need omega dle")};
    ASSERT_TRUE(liquid::search_detail::publish_full(index, "working", "one", documents));

    auto hits = liquid::search_detail::query(index, "working", "\"needle omega\"", 20);
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 1);
    EXPECT_EQ(hits->front().path, "match.txt");
}

TEST(SearchIndex, delta_catalogs_a_blob_reintroduced_from_an_inactive_generation) {
    temporary_directory location;
    const auto index = location.path() / "index";
    const auto old = document("old.txt", "reintroduced needle");
    ASSERT_TRUE(liquid::search_detail::publish_full(index, "working", "one", {old}));
    ASSERT_TRUE(liquid::search_detail::publish_full(
        index, "working", "two", {document("current.txt", "current value")}));
    ASSERT_TRUE(liquid::search_detail::publish_delta(
        index, "working", "three", {liquid::search_detail::document_update{"restored.txt", old}}));

    auto hits = liquid::search_detail::query(index, "working", "\"needle\"", 20);
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 1);
    EXPECT_EQ(hits->front().path, "restored.txt");
}

TEST(SearchIndex, regex_alternatives_do_not_use_an_unsafe_literal_prefilter) {
    temporary_directory location;
    const auto index = location.path() / "index";
    const auto documents = std::vector{document("foo.txt", "only foo appears"),
                                       document("bar.txt", "only bar appears")};
    ASSERT_TRUE(liquid::search_detail::publish_full(index, "working", "one", documents));

    auto hits = liquid::search_detail::query(index, "working", "/foo|bar/", 20);
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 2);
}

TEST(SearchIndex, scan_and_trigram_results_are_identical) {
    temporary_directory location;
    const auto index = location.path() / "index";
    const auto documents =
        std::vector{document("one.txt", "first line\nalpha needle omega\nlast line"),
                    document("two.txt", "alpha other value"),
                    document("three.txt", "prefix needle suffix")};
    ASSERT_TRUE(liquid::search_detail::publish_full(index, "working", "one", documents));

    for (const auto& query : {std::string{"\"needle\""},
                              std::string{"/needle.*omega/"},
                              std::string{"/\\x6eeedle/"},
                              std::string{"/needle (omega|suffix)/"}}) {
        auto indexed = liquid::search_detail::query(
            index, "working", query, 20, liquid::search_executor::trigram);
        auto scanned = liquid::search_detail::scan(documents, query, 20);
        ASSERT_TRUE(indexed) << (indexed ? "" : indexed.error().message);
        ASSERT_TRUE(scanned) << (scanned ? "" : scanned.error().message);
        ASSERT_EQ(indexed->size(), scanned->size());
        for (auto position = std::size_t{}; position != indexed->size(); ++position) {
            EXPECT_EQ(hit_identity((*indexed)[position]), hit_identity((*scanned)[position]));
        }
    }
    auto core = liquid::search_detail::classify("/needle.*omega/");
    auto no_core = liquid::search_detail::classify("/(needle|omega)/");
    auto escaped_core = liquid::search_detail::classify("/\\x6eeedle/");
    ASSERT_TRUE(core);
    ASSERT_TRUE(no_core);
    ASSERT_TRUE(escaped_core);
    EXPECT_EQ(*core, liquid::search_query_kind::regex_with_literal_core);
    EXPECT_EQ(*no_core, liquid::search_query_kind::regex_without_literal_core);
    EXPECT_EQ(*escaped_core, liquid::search_query_kind::regex_without_literal_core);
}

TEST(SearchPlanner, chooses_scan_and_index_on_opposite_sides_of_crossover) {
    auto small = liquid::search_detail::planner_inputs{
        .working_set = {.paths = 10, .unique_blobs = 10, .bytes = 120000},
        .scan_nanoseconds_per_blob = 20000,
        .scan_parallelism = 16,
        .trigram_selectivity = 0.001,
        .index_available = true};
    auto large = small;
    large.working_set = {.paths = 4000, .unique_blobs = 4000, .bytes = 48000000};

    for (const auto kind :
         {liquid::search_query_kind::literal, liquid::search_query_kind::regex_with_literal_core}) {
        const auto small_plan = liquid::search_detail::make_plan(small, kind);
        const auto large_plan = liquid::search_detail::make_plan(large, kind);
        EXPECT_EQ(small_plan.executor, liquid::search_executor::scan);
        EXPECT_EQ(large_plan.executor, liquid::search_executor::trigram);
    }

    large.drips_since_index_update = 1;
    const auto stale_plan =
        liquid::search_detail::make_plan(large, liquid::search_query_kind::literal);
    EXPECT_EQ(stale_plan.executor, liquid::search_executor::scan);
    EXPECT_FALSE(stale_plan.alternatives[1].eligible);
}

TEST(SearchPlanner, counts_capture_drips_after_the_index_cursor) {
    temporary_directory location;
    const auto index = location.path() / ".liquid" / "index";
    const auto capture = location.path() / ".liquid" / "capture";
    std::filesystem::create_directories(capture);
    ASSERT_TRUE(liquid::search_detail::publish_full(
        index, "working", "0:0", {document("one.txt", "fresh before drip")}));
    {
        auto journal = std::ofstream{capture / "drips.cbor", std::ios::binary};
        const auto record = std::array<char, 5>{0, 0, 0, 1, 'x'};
        journal.write(record.data(), static_cast<std::streamsize>(record.size()));
    }
    {
        auto cursor = std::ofstream{capture / "cursor"};
        cursor << "5 1\n";
    }
    auto stale = liquid::search_detail::freshness(index, "working");
    ASSERT_TRUE(stale) << (stale ? "" : stale.error().message);
    EXPECT_EQ(*stale, 1);
}

TEST(SearchIndex, manifest_replacement_is_atomic_for_concurrent_queries) {
    temporary_directory location;
    const auto index = location.path() / "index";
    const auto old_documents =
        std::vector{document("a.txt", "atomic old value"), document("b.txt", "atomic old value")};
    const auto new_documents =
        std::vector{document("a.txt", "atomic new value"), document("b.txt", "atomic new value")};
    ASSERT_TRUE(liquid::search_detail::publish_full(index, "working", "initial", old_documents));

    auto failed = std::atomic<bool>{false};
    auto publisher = std::jthread{[&] {
        for (auto iteration = 0; iteration != 100; ++iteration) {
            const auto& selected = iteration % 2 == 0 ? new_documents : old_documents;
            const auto generation = "generation-" + std::to_string(iteration);
            if (!liquid::search_detail::publish_full(index, "working", generation, selected)) {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    }};
    for (auto iteration = 0; iteration != 300; ++iteration) {
        auto hits = liquid::search_detail::query(index, "working", "\"atomic\"", 20);
        ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
        ASSERT_EQ(hits->size(), 2);
        const auto old_count = std::ranges::count_if(
            *hits, [](const auto& hit) { return hit.snippet.contains("old"); });
        const auto new_count = std::ranges::count_if(
            *hits, [](const auto& hit) { return hit.snippet.contains("new"); });
        EXPECT_TRUE((old_count == 2 && new_count == 0) || (old_count == 0 && new_count == 2));
    }
    publisher.join();
    EXPECT_FALSE(failed.load(std::memory_order_acquire));
}

#ifndef _WIN32

TEST(SearchIndex, daemon_drip_indexes_new_working_tree_content) {
    temporary_directory location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << (created ? "" : created.error().message);
    auto options = liquid::daemon_options{};
    options.idle_timeout = std::chrono::seconds{5};
    options.capture.coalesce_window = std::chrono::milliseconds{5};
    options.capture.reconcile_interval = std::chrono::milliseconds{0};
    options.capture.scan = false;
    auto daemon = liquid::runtime::block_on(liquid::daemon::start(location.path(), options));
    ASSERT_TRUE(daemon) << (daemon ? "" : daemon.error().message);
    auto before = liquid::runtime::block_on(daemon->status());
    ASSERT_TRUE(before);
    const auto search =
        std::ranges::find(before->targets, "search", &liquid::daemon_target_status::name);
    ASSERT_NE(search, before->targets.end());

    {
        auto output = std::ofstream{location.path() / "new-source.cpp"};
        output << "int liquid_drip_search_marker = 22;\n";
    }
    auto delivered = wait_for_search_delivery(*daemon, search->deliveries);
    ASSERT_TRUE(delivered) << (delivered ? "" : delivered.error().message);

    auto opened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(opened) << (opened ? "" : opened.error().message);
    auto hits = liquid::runtime::block_on(opened->search("liquid_drip_search_marker"));
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 1);
    EXPECT_EQ(hits->front().path, "new-source.cpp");
    EXPECT_TRUE(liquid::runtime::block_on(daemon->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(daemon->wait()));
}

#endif

TEST(SearchIndex, duplicate_cursor_advances_search_target_once) {
    temporary_directory location;
    {
        auto output = std::ofstream{location.path() / "source.txt"};
        output << "duplicate delivery marker\n";
    }
    const auto target = liquid::search_detail::make_daemon_target(location.path());
    auto fanout =
        liquid::daemon_detail::fanout{location.path() / "state.cbor",
                                      std::vector<std::shared_ptr<liquid::daemon_target>>{target}};
    ASSERT_TRUE(fanout.load());
    const auto delivery = liquid::daemon_delivery{
        .value = {.id = {fingerprint("duplicate")}, .ops = {{"source.txt"}}}, .cursor = {"42:0"}};
    EXPECT_TRUE(liquid::runtime::block_on(fanout.deliver(delivery)));
    EXPECT_TRUE(liquid::runtime::block_on(fanout.deliver(delivery)));
    const auto status = fanout.target_status({"42:0"});
    ASSERT_EQ(status.size(), 1);
    EXPECT_EQ(status.front().cursor.token, "42:0");
    EXPECT_EQ(status.front().deliveries, 1);

    auto hits = liquid::search_detail::query(
        location.path() / ".liquid" / "index", "working", "duplicate", 20);
    ASSERT_TRUE(hits) << (hits ? "" : hits.error().message);
    ASSERT_EQ(hits->size(), 1);
}

TEST(SearchIndex, daemonTargetReplacesArchiveEntryDocuments) {
    temporary_directory location;
    const auto archive_path = location.path() / "archive.zip";
    auto write_archive = [&](std::string_view name) {
        const auto contents = format_fixture(name);
        auto output = std::ofstream{archive_path, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        return output.good();
    };
    ASSERT_TRUE(write_archive("archive-a.zip"));
    const auto target = liquid::search_detail::make_daemon_target(location.path());
    auto delivered = liquid::runtime::block_on(
        target->deliver({.value = {.id = {fingerprint("archive-a")}, .ops = {{"archive.zip"}}},
                         .cursor = {"1:0"}}));
    ASSERT_TRUE(delivered) << delivered.error().message;

    const auto index = location.path() / ".liquid" / "index";
    auto old_hits =
        liquid::search_detail::query(index, "working", "\"original archive value\"", 20);
    ASSERT_TRUE(old_hits) << old_hits.error().message;
    ASSERT_EQ(old_hits->size(), 1);
    EXPECT_EQ(old_hits->front().path, "archive.zip!/deflated.txt");

    ASSERT_TRUE(write_archive("archive-b.zip"));
    delivered = liquid::runtime::block_on(
        target->deliver({.value = {.id = {fingerprint("archive-b")}, .ops = {{"archive.zip"}}},
                         .cursor = {"2:0"}}));
    ASSERT_TRUE(delivered) << delivered.error().message;
    old_hits = liquid::search_detail::query(index, "working", "\"original archive value\"", 20);
    ASSERT_TRUE(old_hits) << old_hits.error().message;
    EXPECT_TRUE(old_hits->empty());
    auto new_hits = liquid::search_detail::query(index, "working", "\"changed archive value\"", 20);
    ASSERT_TRUE(new_hits) << new_hits.error().message;
    ASSERT_EQ(new_hits->size(), 1);
    EXPECT_EQ(new_hits->front().path, "archive.zip!/deflated.txt");
}
