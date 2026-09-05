#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "daemon/fanout.hpp"
#include "liquid/daemon.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "search/index.hpp"

namespace {

static_assert(std::is_aggregate_v<liquid::search_options>);
static_assert(std::is_aggregate_v<liquid::search_hit>);

class temporary_directory {
public:
    temporary_directory()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-search-test-" + std::to_string(next_.fetch_add(1)))) {
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
