// A repository's own state goes through store_backend (liquid #326): LIQUID_REPO, LIQUID_JOURNAL,
// session state and replication cursors are backend keys, so a backend with no local root holds
// every byte of a shared-store repository. These tests run the repository lifecycle against the
// in-memory backend and the disk backend, including a seeded crash schedule over every fault point.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../temporary_directory.hpp"
#include "faults.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "memory_backend.hpp"

namespace {

using liquid::store_detail::fault_event;
using liquid::store_detail::fault_injector;

enum class backend_kind { disk, memory };

std::string backend_name(const testing::TestParamInfo<backend_kind>& parameter) {
    return parameter.param == backend_kind::disk ? "Disk" : "Memory";
}

std::vector<std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

std::uint64_t environment_number(const char* name, std::uint64_t fallback) {
    const auto* text = std::getenv(name);
    if (!text || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        throw std::runtime_error(std::string(name) + " must be an unsigned decimal integer");
    }
    return value;
}

class scoped_directory {
public:
    explicit scoped_directory(std::string_view name)
        : path_(liquid::test::fixture_path(std::string{name})) {
        std::filesystem::remove_all(path_);
    }
    ~scoped_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }
    scoped_directory(const scoped_directory&) = delete;
    scoped_directory& operator=(const scoped_directory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class injector_guard {
public:
    explicit injector_guard(fault_injector* injector)
        : previous_(liquid::store_detail::set_fault_injector(injector)) {}
    ~injector_guard() {
        liquid::store_detail::set_fault_injector(previous_);
    }
    injector_guard(const injector_guard&) = delete;
    injector_guard& operator=(const injector_guard&) = delete;

private:
    fault_injector* previous_{};
};

class simulated_crash final : public std::exception {
public:
    const char* what() const noexcept override {
        return "simulated repository crash";
    }
};

// Counts fault points, or throws at one of them. On firing it runs the freeze callback first so a
// memory backend refuses everything the crashed process would never have written.
class crash_injector final : public fault_injector {
public:
    explicit crash_injector(std::uint64_t target, std::function<void()> on_crash = {})
        : target_(target), on_crash_(std::move(on_crash)) {}

    void visit(fault_event event) override {
        const auto step = step_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (target_ == 0) {
            const auto path = event.path.generic_string();
            if (path.find("repositories/") != std::string::npos) {
                auto lock = std::scoped_lock{mutex_};
                state_steps_.push_back(step);
            }
            return;
        }
        if (step != target_) {
            return;
        }
        fired_.store(true, std::memory_order_relaxed);
        where_ = event.path.generic_string();
        if (on_crash_) {
            on_crash_();
        }
        throw simulated_crash{};
    }

    [[nodiscard]] std::uint64_t steps() const noexcept {
        return step_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool fired() const noexcept {
        return fired_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const std::string& where() const noexcept {
        return where_;
    }
    // The counted fault points that touch repository state rather than pack or index data.
    [[nodiscard]] std::vector<std::uint64_t> state_steps() const {
        auto lock = std::scoped_lock{mutex_};
        return state_steps_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> state_steps_;
    std::uint64_t target_{};
    std::function<void()> on_crash_;
    std::atomic<std::uint64_t> step_{};
    std::atomic<bool> fired_{};
    std::string where_;
};

// Fires at the first fault point whose path names the given state object, in the given phase.
class named_crash_injector final : public fault_injector {
public:
    named_crash_injector(std::string name,
                         liquid::store_detail::fault_phase phase,
                         std::function<void()> on_crash)
        : name_(std::move(name)), phase_(phase), on_crash_(std::move(on_crash)) {}

    void visit(fault_event event) override {
        if (fired_ || event.phase != phase_ ||
            event.path.generic_string().find(name_) == std::string::npos) {
            return;
        }
        fired_ = true;
        if (on_crash_) {
            on_crash_();
        }
        throw simulated_crash{};
    }

    [[nodiscard]] bool fired() const noexcept {
        return fired_;
    }

private:
    std::string name_;
    liquid::store_detail::fault_phase phase_{};
    std::function<void()> on_crash_;
    bool fired_{};
};

// One shared store on a backend, reopened as a process restart would.
struct shared_store_host {
    backend_kind kind{};
    std::filesystem::path root;
    std::shared_ptr<liquid::store_backend> backend;
    std::shared_ptr<liquid::sim::memory_store_backend> memory;
    std::optional<liquid::store> objects;

    shared_store_host(backend_kind value, std::filesystem::path path)
        : kind(value), root(std::move(path)) {
        if (kind == backend_kind::memory) {
            memory = std::make_shared<liquid::sim::memory_store_backend>();
            backend = memory;
        } else {
            backend = liquid::make_disk_store_backend(root);
        }
    }

    [[nodiscard]] liquid::result<void> open() {
        objects.reset();
        auto options = liquid::store_options{};
        options.pack_size = 64U * 1024U;
        options.backend = backend;
        auto opened = liquid::runtime::block_on(liquid::store::open(root, options));
        if (!opened) {
            return std::unexpected(opened.error());
        }
        objects.emplace(std::move(*opened));
        return {};
    }

    [[nodiscard]] liquid::repository_options repository_options(std::uint64_t interval = 64) {
        auto options = liquid::repository_options{};
        options.store = &*objects;
        options.checkpoint_interval = interval;
        return options;
    }

    void freeze(bool frozen) {
        if (memory) {
            memory->set_frozen(frozen);
        }
    }
};

std::string message(const liquid::error& failure) {
    return failure.message + " (" + failure.context + ")";
}

class StoreBackendConditionalCreate : public testing::TestWithParam<backend_kind> {};

TEST_P(StoreBackendConditionalCreate, OnlyTheFirstCreatorWins) {
    scoped_directory location("liquid-put-if-absent-" + std::string{backend_name({GetParam(), 0})});
    auto backend = GetParam() == backend_kind::disk
                       ? liquid::make_disk_store_backend(location.path())
                       : std::shared_ptr<liquid::store_backend>{
                             std::make_shared<liquid::sim::memory_store_backend>()};
    auto created = liquid::runtime::block_on(
        backend->put_if_absent("repositories/a/LIQUID_REPO", bytes_of("first")));
    ASSERT_TRUE(created) << message(created.error());
    auto refused = liquid::runtime::block_on(
        backend->put_if_absent("repositories/a/LIQUID_REPO", bytes_of("second")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::already_exists);
    auto stored = liquid::runtime::block_on(backend->get_range("repositories/a/LIQUID_REPO", 0, 5));
    ASSERT_TRUE(stored);
    EXPECT_EQ(*stored, bytes_of("first"));

    // Racing creators: exactly one succeeds and no temporary is left behind.
    constexpr auto racers = 8;
    auto winners = std::atomic<int>{};
    auto losers = std::atomic<int>{};
    auto threads = std::vector<std::thread>{};
    for (auto index = 0; index != racers; ++index) {
        threads.emplace_back([&, index] {
            auto put = liquid::runtime::block_on(backend->put_if_absent(
                "repositories/b/LIQUID_REPO", bytes_of("racer-" + std::to_string(index))));
            if (put) {
                winners.fetch_add(1);
            } else if (put.error().code == liquid::errc::already_exists) {
                losers.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(winners.load(), 1);
    EXPECT_EQ(losers.load(), racers - 1);
    auto listed = liquid::runtime::block_on(backend->list("repositories/"));
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 2U);
    if (GetParam() == backend_kind::disk) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(location.path() / "repositories")) {
            EXPECT_FALSE(entry.path().filename().string().ends_with(".backend-tmp"))
                << entry.path();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         StoreBackendConditionalCreate,
                         testing::Values(backend_kind::disk, backend_kind::memory),
                         backend_name);

// A backend that implements only the required operations refuses a conditional create instead of
// emulating it with a racy test-then-put.
class minimal_backend final : public liquid::store_backend {
public:
    liquid::runtime::task<void> put(std::string, std::vector<std::byte>) override {
        co_return liquid::result<void>{};
    }
    liquid::runtime::task<void>
    put_range(std::string, std::uint64_t, std::vector<std::byte>) override {
        co_return liquid::result<void>{};
    }
    liquid::runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t, std::uint64_t) override {
        co_return std::unexpected(liquid::error{liquid::errc::not_found, "absent", key});
    }
    liquid::runtime::task<void> remove(std::string) override {
        co_return liquid::result<void>{};
    }
    liquid::runtime::task<std::vector<liquid::store_backend_entry>> list(std::string) override {
        co_return std::vector<liquid::store_backend_entry>{};
    }
    liquid::runtime::task<void> barrier() override {
        co_return liquid::result<void>{};
    }
};

TEST(StoreBackendConditionalCreateDefault, IsUnsupported) {
    auto backend = minimal_backend{};
    auto refused = liquid::runtime::block_on(backend.put_if_absent("key", bytes_of("value")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unsupported);
}

TEST(DiskStoreBackend, ListReturnsExactlyTheKeysWithThePrefix) {
    scoped_directory location("liquid-disk-list-prefix");
    auto backend = liquid::make_disk_store_backend(location.path());
    for (const auto* key : {"LIQUID_REPO",
                            "LIQUID_JOURNAL",
                            "LIQUID_REPOSITORY_NOTE",
                            "work/LIQUID_REPO",
                            "a/b",
                            "ab",
                            "abc/d",
                            "abc/e/f",
                            "b"}) {
        ASSERT_TRUE(liquid::runtime::block_on(backend->put(key, bytes_of(key))));
    }
    const auto keys = [&](std::string prefix) {
        auto listed = liquid::runtime::block_on(backend->list(std::move(prefix)));
        EXPECT_TRUE(listed);
        auto output = std::vector<std::string>{};
        for (const auto& entry : *listed) {
            output.push_back(entry.key);
        }
        return output;
    };
    EXPECT_EQ(keys("LIQUID_REPO"),
              (std::vector<std::string>{"LIQUID_REPO", "LIQUID_REPOSITORY_NOTE"}));
    EXPECT_EQ(keys("a"), (std::vector<std::string>{"a/b", "ab", "abc/d", "abc/e/f"}));
    EXPECT_EQ(keys("a/"), (std::vector<std::string>{"a/b"}));
    EXPECT_EQ(keys("abc/e"), (std::vector<std::string>{"abc/e/f"}));
    EXPECT_EQ(keys("missing/"), (std::vector<std::string>{}));
    EXPECT_EQ(keys("").size(), 9U);
}

std::optional<std::string> state_key(const liquid::sim::memory_store_backend& backend,
                                     std::string_view suffix) {
    for (const auto& key : backend.keys()) {
        if (key.starts_with("repositories/") && key.ends_with(suffix)) {
            return key;
        }
    }
    return std::nullopt;
}

liquid::result<std::vector<liquid::sync_item>> exported_items(liquid::repository& repository) {
    auto cursor = liquid::journal_cursor{};
    auto output = std::vector<liquid::sync_item>{};
    for (;;) {
        auto page = std::vector<liquid::sync_item>(32);
        auto exported = liquid::runtime::block_on(repository.export_drips(cursor, page));
        if (!exported) {
            return std::unexpected(exported.error());
        }
        output.insert(output.end(), page.begin(), page.begin() + exported->count);
        cursor = exported->cursor;
        if (exported->done) {
            return output;
        }
    }
}

// The issue's acceptance case: create, write, cut, sync, reopen, and collect a shared-store
// repository on a backend with no local root, and find nothing under any directory.
TEST(RepositoryStateBackend, SharedStoreRepositoryLivesEntirelyInTheBackend) {
    const auto nominal = liquid::test::fixture_path("liquid-memory-repository-root");
    std::filesystem::remove_all(nominal);
    auto host = shared_store_host{backend_kind::memory, nominal};
    ASSERT_TRUE(host.open());

    auto created = liquid::runtime::block_on(
        liquid::repository::create("workspace/alpha", host.repository_options(4)));
    ASSERT_TRUE(created) << message(created.error());
    auto duplicate = liquid::runtime::block_on(
        liquid::repository::create("workspace/alpha", host.repository_options(4)));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, liquid::errc::already_exists);

    ASSERT_TRUE(liquid::runtime::block_on(created->write("README", bytes_of("alpha"))));
    auto first = liquid::runtime::block_on(created->cut("first"));
    ASSERT_TRUE(first) << message(first.error());
    for (auto index = 0; index != 6; ++index) {
        ASSERT_TRUE(liquid::runtime::block_on(created->write(
            "notes/" + std::to_string(index), bytes_of("note " + std::to_string(index)))));
    }
    ASSERT_TRUE(liquid::runtime::block_on(created->write("journal-only", bytes_of("pending"))));
    ASSERT_TRUE(state_key(*host.memory, "/LIQUID_REPO"));
    ASSERT_TRUE(state_key(*host.memory, "/LIQUID_JOURNAL"))
        << "a write between checkpoints publishes the journal";

    auto invalid = liquid::runtime::block_on(
        created->set_replication_cursor("../escape", liquid::journal_cursor{"1"}));
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, liquid::errc::invalid_argument);
    auto unset = liquid::runtime::block_on(created->replication_cursor("peer-a"));
    ASSERT_TRUE(unset);
    EXPECT_FALSE(unset->has_value());
    ASSERT_TRUE(liquid::runtime::block_on(
        created->set_replication_cursor("peer-a", liquid::journal_cursor{"7:0"})));
    ASSERT_TRUE(state_key(*host.memory, "/replication/peer-a.cursor"));

    auto replica = liquid::runtime::block_on(
        liquid::repository::create("workspace/replica", host.repository_options()));
    ASSERT_TRUE(replica) << message(replica.error());
    auto items = exported_items(*created);
    ASSERT_TRUE(items) << message(items.error());
    auto imported = liquid::runtime::block_on(replica->import_drips(*items));
    ASSERT_TRUE(imported) << message(imported.error());
    auto replicated = liquid::runtime::block_on(replica->read("README"));
    ASSERT_TRUE(replicated) << message(replicated.error());
    EXPECT_EQ(*replicated, bytes_of("alpha"));

    *created = {};
    *replica = {};
    ASSERT_TRUE(host.open());
    auto reopened = liquid::runtime::block_on(
        liquid::repository::open("workspace/alpha", host.repository_options(4)));
    ASSERT_TRUE(reopened) << message(reopened.error());
    auto journal = liquid::runtime::block_on(reopened->read("journal-only"));
    ASSERT_TRUE(journal) << message(journal.error());
    EXPECT_EQ(*journal, bytes_of("pending"));
    auto note = liquid::runtime::block_on(reopened->read("notes/5"));
    ASSERT_TRUE(note);
    EXPECT_EQ(*note, bytes_of("note 5"));
    auto history = liquid::runtime::block_on(reopened->log());
    ASSERT_TRUE(history);
    EXPECT_TRUE(std::ranges::any_of(*history, [&](const liquid::change& value) {
        return value.id.value.bytes == first->value.bytes;
    }));
    auto cursor = liquid::runtime::block_on(reopened->replication_cursor("peer-a"));
    ASSERT_TRUE(cursor);
    ASSERT_TRUE(cursor->has_value());
    EXPECT_EQ((*cursor)->token, "7:0");
    // Opening checkpoints, which retires the journal.
    EXPECT_FALSE(state_key(*host.memory, "/LIQUID_JOURNAL"));

    auto collected = liquid::runtime::block_on(reopened->collect());
    ASSERT_TRUE(collected) << message(collected.error());
    auto after = liquid::runtime::block_on(reopened->read("README"));
    ASSERT_TRUE(after) << message(after.error());
    EXPECT_EQ(*after, bytes_of("alpha"));

    auto missing = liquid::runtime::block_on(
        liquid::repository::open("workspace/never-created", host.repository_options()));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);

    for (const auto& key : host.memory->keys()) {
        const auto known = key == "LIQUID_STORE" || key.starts_with("packs/") ||
                           key.starts_with("index/") || key.starts_with("leases/") ||
                           key.starts_with("gc/") || key.starts_with("repositories/");
        EXPECT_TRUE(known) << key;
    }
    EXPECT_FALSE(std::filesystem::exists(nominal)) << "nothing may be written under any directory";
}

// The disk backend keeps the layout repositories already have on disk: a shared-store repository's
// state is <store>/repositories/<id-hash>/LIQUID_REPO, a checkout's is <checkout>/LIQUID_REPO, and
// a checkout carrying a temporary left by an earlier release's interrupted marker rename opens.
// (Inline legacy journals are covered in test_repository.cpp.)
TEST(RepositoryStateBackend, DiskLayoutIsUnchangedAndEarlierStateOpens) {
    scoped_directory location("liquid-disk-repository-layout");
    auto host = shared_store_host{backend_kind::disk, location.path() / "store"};
    ASSERT_TRUE(host.open());
    auto shared = liquid::runtime::block_on(
        liquid::repository::create("workspace/alpha", host.repository_options()));
    ASSERT_TRUE(shared) << message(shared.error());
    ASSERT_TRUE(liquid::runtime::block_on(shared->write("file", bytes_of("shared"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        shared->set_replication_cursor("peer-a", liquid::journal_cursor{"3:0"})));
    *shared = {};
    auto directories = std::vector<std::filesystem::path>{};
    for (const auto& entry :
         std::filesystem::directory_iterator(location.path() / "store" / "repositories")) {
        directories.push_back(entry.path());
    }
    ASSERT_EQ(directories.size(), 1U);
    EXPECT_TRUE(std::filesystem::is_regular_file(directories.front() / "LIQUID_REPO"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(directories.front() / "replication" / "peer-a.cursor"));

    const auto checkout = location.path() / "checkout";
    auto local = liquid::runtime::block_on(liquid::repository::create(checkout));
    ASSERT_TRUE(local) << message(local.error());
    ASSERT_TRUE(liquid::runtime::block_on(local->write("file", bytes_of("local"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        local->set_replication_cursor("peer-a", liquid::journal_cursor{"5:0"})));
    *local = {};
    EXPECT_TRUE(std::filesystem::is_regular_file(checkout / "LIQUID_REPO"));
    EXPECT_TRUE(std::filesystem::is_regular_file(checkout / "LIQUID_JOURNAL"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(checkout / ".liquid" / "replication" / "peer-a.cursor"));

    // Rewrite the state as earlier releases left it after a crash between their temporary write
    // and rename: the marker in place plus a stale LIQUID_REPO.tmp beside it.
    {
        auto stale = std::ofstream{checkout / "LIQUID_REPO.tmp", std::ios::binary};
        stale << "partial";
    }
    auto reopened = liquid::runtime::block_on(liquid::repository::open(checkout));
    ASSERT_TRUE(reopened) << message(reopened.error());
    auto bytes = liquid::runtime::block_on(reopened->read("file"));
    ASSERT_TRUE(bytes) << message(bytes.error());
    EXPECT_EQ(*bytes, bytes_of("local"));
    auto cursor = liquid::runtime::block_on(reopened->replication_cursor("peer-a"));
    ASSERT_TRUE(cursor);
    ASSERT_TRUE(cursor->has_value());
    EXPECT_EQ((*cursor)->token, "5:0");
    *reopened = {};

    ASSERT_TRUE(host.open());
    auto shared_again = liquid::runtime::block_on(
        liquid::repository::open("workspace/alpha", host.repository_options()));
    ASSERT_TRUE(shared_again) << message(shared_again.error());
    auto shared_bytes = liquid::runtime::block_on(shared_again->read("file"));
    ASSERT_TRUE(shared_bytes);
    EXPECT_EQ(*shared_bytes, bytes_of("shared"));
}

class RepositoryJournalCrash
    : public testing::TestWithParam<std::tuple<backend_kind, liquid::store_detail::fault_phase>> {};

// A crash while the journal is being published leaves the repository openable, with every
// acknowledged write and the interrupted write either wholly present or wholly absent.
TEST_P(RepositoryJournalCrash, ReopenRecoversAcknowledgedWrites) {
    const auto [kind, phase] = GetParam();
    scoped_directory location(
        std::string{"liquid-journal-crash-"} + (kind == backend_kind::disk ? "disk-" : "memory-") +
        (phase == liquid::store_detail::fault_phase::before ? "before" : "after"));
    auto host = shared_store_host{kind, location.path() / "store"};
    ASSERT_TRUE(host.open());
    auto subject = liquid::runtime::block_on(
        liquid::repository::create("workspace/alpha", host.repository_options(1000)));
    ASSERT_TRUE(subject) << message(subject.error());
    ASSERT_TRUE(liquid::runtime::block_on(subject->write("kept", bytes_of("acknowledged"))));
    ASSERT_TRUE(liquid::runtime::block_on(subject->write("kept-2", bytes_of("acknowledged 2"))));

    auto crash = named_crash_injector{"LIQUID_JOURNAL", phase, [&] { host.freeze(true); }};
    {
        auto guard = injector_guard{&crash};
        auto interrupted =
            liquid::runtime::block_on(subject->write("interrupted", bytes_of("in flight")));
        EXPECT_FALSE(interrupted);
    }
    ASSERT_TRUE(crash.fired());
    *subject = {};
    host.objects.reset();
    host.freeze(false);

    ASSERT_TRUE(host.open());
    auto reopened = liquid::runtime::block_on(
        liquid::repository::open("workspace/alpha", host.repository_options(1000)));
    ASSERT_TRUE(reopened) << message(reopened.error());
    auto kept = liquid::runtime::block_on(reopened->read("kept"));
    ASSERT_TRUE(kept) << message(kept.error());
    EXPECT_EQ(*kept, bytes_of("acknowledged"));
    auto kept_2 = liquid::runtime::block_on(reopened->read("kept-2"));
    ASSERT_TRUE(kept_2) << message(kept_2.error());
    EXPECT_EQ(*kept_2, bytes_of("acknowledged 2"));
    auto interrupted = liquid::runtime::block_on(reopened->read("interrupted"));
    if (interrupted) {
        EXPECT_EQ(*interrupted, bytes_of("in flight"));
    } else {
        EXPECT_EQ(interrupted.error().code, liquid::errc::not_found);
    }
    ASSERT_TRUE(liquid::runtime::block_on(reopened->write("after", bytes_of("usable"))));
}

INSTANTIATE_TEST_SUITE_P(
    BackendsAndPhases,
    RepositoryJournalCrash,
    testing::Combine(testing::Values(backend_kind::disk, backend_kind::memory),
                     testing::Values(liquid::store_detail::fault_phase::before,
                                     liquid::store_detail::fault_phase::after)),
    [](const auto& parameter) {
        return std::string{std::get<0>(parameter.param) == backend_kind::disk ? "Disk" : "Memory"} +
               (std::get<1>(parameter.param) == liquid::store_detail::fault_phase::before
                    ? "Before"
                    : "After");
    });

// liquid #337: a collection publishes a new object map and commits a collection manifest whose
// tombstones mark the collected chunks dead. Reopening after a crash between the two must find
// every chunk the surviving object map names. Seed 17 of the repository simulation reached that
// point only when wall-clock timestamps in drips and changes happened to shift the fault points
// onto it; this test crashes the same collection at each fault point of that publication: every
// one that touches the object map or the collection manifest, and the one after each.

// Records the path of every fault point, in order.
class recording_injector final : public fault_injector {
public:
    void visit(fault_event event) override {
        auto lock = std::scoped_lock{mutex_};
        paths_.push_back(event.path.generic_string());
    }
    [[nodiscard]] std::vector<std::string> paths() const {
        auto lock = std::scoped_lock{mutex_};
        return paths_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> paths_;
};

class RepositoryCollectionCrash : public testing::TestWithParam<backend_kind> {};

// Builds a repository whose collection reclaims blob records: contents overwritten before any cut
// stop being reachable once a later checkpoint no longer names them.
std::optional<std::string> prepare_garbage(shared_store_host& host,
                                           std::optional<liquid::repository>& subject) {
    auto opened = host.open();
    if (!opened) {
        return "store open failed: " + message(opened.error());
    }
    auto created = liquid::runtime::block_on(
        liquid::repository::create("workspace/alpha", host.repository_options(3)));
    if (!created) {
        return "repository create failed: " + message(created.error());
    }
    subject.emplace(std::move(*created));
    if (!liquid::runtime::block_on(subject->write("kept", bytes_of("acknowledged"))) ||
        !liquid::runtime::block_on(subject->cut("kept"))) {
        return "could not write the kept file";
    }
    for (auto round = 0; round != 8; ++round) {
        if (!liquid::runtime::block_on(subject->write(
                "scratch", bytes_of("scratch contents, round " + std::to_string(round))))) {
            return "could not overwrite the scratch file";
        }
    }
    return std::nullopt;
}

TEST_P(RepositoryCollectionCrash, ReopenAfterACrashAtEveryCollectionFaultPoint) {
    const auto kind = GetParam();
    scoped_directory location("liquid-collection-crash-" + backend_name({kind, 0}));
    auto paths = std::vector<std::string>{};
    {
        auto host = shared_store_host{kind, location.path() / "probe"};
        auto subject = std::optional<liquid::repository>{};
        const auto prepared = prepare_garbage(host, subject);
        ASSERT_FALSE(prepared) << *prepared;
        auto recorder = recording_injector{};
        {
            auto guard = injector_guard{&recorder};
            auto collected = liquid::runtime::block_on(subject->collect());
            ASSERT_TRUE(collected) << message(collected.error());
            EXPECT_GT(collected->dead_chunks, 0U) << "the collection must reclaim something";
        }
        paths = recorder.paths();
    }
    const auto steps = static_cast<std::uint64_t>(paths.size());
    auto targets = std::set<std::uint64_t>{};
    for (auto index = std::size_t{}; index != paths.size(); ++index) {
        if (paths[index].find("packs/objects.meta") != std::string::npos ||
            paths[index].find("gc/MANIFEST") != std::string::npos) {
            targets.insert(index + 1);
            if (index + 2 <= paths.size()) {
                targets.insert(index + 2);
            }
        }
    }
    ASSERT_GE(targets.size(), 4U) << "the collection published no object map and manifest";
    for (const auto target : targets) {
        SCOPED_TRACE("crash at collection fault point " + std::to_string(target) + " of " +
                     std::to_string(steps));
        const auto root = location.path() / ("actual-" + std::to_string(target));
        auto host = shared_store_host{kind, root};
        auto subject = std::optional<liquid::repository>{};
        const auto prepared = prepare_garbage(host, subject);
        ASSERT_FALSE(prepared) << *prepared;
        auto crash = crash_injector{target, [&] { host.freeze(true); }};
        {
            auto guard = injector_guard{&crash};
            auto collected = liquid::runtime::block_on(subject->collect());
            static_cast<void>(collected);
        }
        subject.reset();
        host.objects.reset();
        host.freeze(false);

        auto opened = host.open();
        ASSERT_TRUE(opened) << crash.where() << ": " << message(opened.error());
        auto reopened = liquid::runtime::block_on(
            liquid::repository::open("workspace/alpha", host.repository_options(3)));
        ASSERT_TRUE(reopened) << crash.where() << ": " << message(reopened.error());
        auto kept = liquid::runtime::block_on(reopened->read("kept"));
        ASSERT_TRUE(kept) << crash.where() << ": " << message(kept.error());
        EXPECT_EQ(*kept, bytes_of("acknowledged"));
        auto scratch = liquid::runtime::block_on(reopened->read("scratch"));
        ASSERT_TRUE(scratch) << crash.where() << ": " << message(scratch.error());
        EXPECT_EQ(*scratch, bytes_of("scratch contents, round 7"));
        auto collected = liquid::runtime::block_on(reopened->collect());
        ASSERT_TRUE(collected) << crash.where() << ": " << message(collected.error());
        ASSERT_TRUE(liquid::runtime::block_on(reopened->write("after", bytes_of("usable"))));
        *reopened = {};
        host.objects.reset();
        std::filesystem::remove_all(root);
    }
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         RepositoryCollectionCrash,
                         testing::Values(backend_kind::disk, backend_kind::memory),
                         backend_name);

// The seeded crash schedule for repositories: one schedule of writes, cuts, replication-cursor
// updates, reopens, and collections runs once to count fault points, then again with a crash at
// one of them chosen by the seed. After the crash the store and repository reopen, every
// acknowledged write, change, and cursor is intact, the interrupted operation is wholly present or
// wholly absent, and the repository accepts new work.
enum class repository_operation { write, cut, cursor, reopen, collect };

struct scheduled_operation {
    repository_operation kind{};
    std::string path;
    std::vector<std::byte> bytes;
};

struct acknowledged_state {
    std::map<std::string, std::vector<std::byte>> files;
    std::vector<liquid::change_id> changes;
    std::optional<std::string> cursor;
};

std::vector<scheduled_operation> make_repository_schedule(std::mt19937_64& random) {
    auto schedule = std::vector<scheduled_operation>{};
    for (auto index = 0; index != 14; ++index) {
        const auto choice = random() % 10;
        if (choice < 5) {
            const auto path =
                "dir-" + std::to_string(random() % 3) + "/file-" + std::to_string(random() % 5);
            auto bytes = std::vector<std::byte>(16 + random() % 6000);
            for (auto& value : bytes) {
                value = std::byte{static_cast<unsigned char>(random() & 0xffU)};
            }
            schedule.push_back({repository_operation::write, path, std::move(bytes)});
        } else if (choice < 7) {
            schedule.push_back({repository_operation::cut, "cut " + std::to_string(index), {}});
        } else if (choice == 7) {
            schedule.push_back(
                {repository_operation::cursor, std::to_string(random() % 1000) + ":0", {}});
        } else if (choice == 8) {
            schedule.push_back({repository_operation::reopen, {}, {}});
        } else {
            schedule.push_back({repository_operation::collect, {}, {}});
        }
    }
    return schedule;
}

struct schedule_outcome {
    std::optional<std::string> failure;
    std::optional<scheduled_operation> interrupted;
};

schedule_outcome run_repository_schedule(shared_store_host& host,
                                         std::optional<liquid::repository>& subject,
                                         const std::vector<scheduled_operation>& schedule,
                                         acknowledged_state& acknowledged,
                                         const crash_injector& crash) {
    for (const auto& item : schedule) {
        auto failed = std::optional<liquid::error>{};
        switch (item.kind) {
        case repository_operation::write: {
            auto written = liquid::runtime::block_on(subject->write(item.path, item.bytes));
            if (written) {
                acknowledged.files.insert_or_assign(item.path, item.bytes);
            } else {
                failed = written.error();
            }
            break;
        }
        case repository_operation::cut: {
            auto cut = liquid::runtime::block_on(subject->cut(item.path));
            if (cut) {
                acknowledged.changes.push_back(*cut);
            } else {
                failed = cut.error();
            }
            break;
        }
        case repository_operation::cursor: {
            auto set = liquid::runtime::block_on(
                subject->set_replication_cursor("peer", liquid::journal_cursor{item.path}));
            if (set) {
                acknowledged.cursor = item.path;
            } else {
                failed = set.error();
            }
            break;
        }
        case repository_operation::reopen: {
            subject.reset();
            auto opened = host.open();
            if (opened) {
                auto reopened = liquid::runtime::block_on(
                    liquid::repository::open("workspace/alpha", host.repository_options(3)));
                if (reopened) {
                    subject.emplace(std::move(*reopened));
                } else {
                    failed = reopened.error();
                }
            } else {
                failed = opened.error();
            }
            break;
        }
        case repository_operation::collect: {
            auto collected = liquid::runtime::block_on(subject->collect());
            if (!collected) {
                failed = collected.error();
            }
            break;
        }
        }
        if (crash.fired()) {
            return {std::nullopt, item};
        }
        if (failed) {
            return {"operation failed: " + message(*failed), std::nullopt};
        }
    }
    return {};
}

std::optional<std::string> verify_recovered(shared_store_host& host,
                                            const acknowledged_state& acknowledged,
                                            const scheduled_operation& interrupted) {
    auto opened = host.open();
    if (!opened) {
        return "store reopen failed: " + message(opened.error());
    }
    auto subject = liquid::runtime::block_on(
        liquid::repository::open("workspace/alpha", host.repository_options(3)));
    if (!subject) {
        return "repository reopen failed: " + message(subject.error());
    }
    for (const auto& [path, expected] : acknowledged.files) {
        auto read = liquid::runtime::block_on(subject->read(path));
        if (!read) {
            return "acknowledged write " + path + " is unreadable: " + message(read.error());
        }
        const auto interrupted_here =
            interrupted.kind == repository_operation::write && interrupted.path == path;
        if (*read != expected && !(interrupted_here && *read == interrupted.bytes)) {
            return "acknowledged write " + path + " returned other bytes";
        }
    }
    if (interrupted.kind == repository_operation::write &&
        !acknowledged.files.contains(interrupted.path)) {
        auto read = liquid::runtime::block_on(subject->read(interrupted.path));
        if (read && *read != interrupted.bytes) {
            return "interrupted write " + interrupted.path + " is torn";
        }
        if (!read && read.error().code != liquid::errc::not_found) {
            return "interrupted write " + interrupted.path +
                   " failed unexpectedly: " + message(read.error());
        }
    }
    auto history = liquid::runtime::block_on(subject->log());
    if (!history) {
        return "log failed: " + message(history.error());
    }
    for (const auto& change : acknowledged.changes) {
        const auto found = std::ranges::any_of(*history, [&](const liquid::change& value) {
            return value.id.value.bytes == change.value.bytes;
        });
        if (!found) {
            return "an acknowledged change is missing from the log";
        }
    }
    auto cursor = liquid::runtime::block_on(subject->replication_cursor("peer"));
    if (!cursor) {
        return "replication cursor read failed: " + message(cursor.error());
    }
    const auto recovered = *cursor ? std::optional<std::string>{(*cursor)->token} : std::nullopt;
    const auto interrupted_cursor = interrupted.kind == repository_operation::cursor
                                        ? std::optional<std::string>{interrupted.path}
                                        : std::nullopt;
    if (recovered != acknowledged.cursor && recovered != interrupted_cursor) {
        return "replication cursor is neither the acknowledged nor the interrupted value";
    }
    auto written = liquid::runtime::block_on(subject->write("recovered", bytes_of("usable")));
    if (!written) {
        return "repository is not writable after recovery: " + message(written.error());
    }
    auto cut = liquid::runtime::block_on(subject->cut("after recovery"));
    if (!cut) {
        return "repository cannot cut after recovery: " + message(cut.error());
    }
    auto collected = liquid::runtime::block_on(subject->collect());
    if (!collected) {
        return "collection failed after recovery: " + message(collected.error());
    }
    for (const auto& [path, expected] : acknowledged.files) {
        auto read = liquid::runtime::block_on(subject->read(path));
        if (!read) {
            return "collection lost acknowledged write " + path + ": " + message(read.error());
        }
    }
    return std::nullopt;
}

std::optional<std::string> run_repository_simulation(std::uint64_t seed,
                                                     backend_kind kind,
                                                     const std::filesystem::path& workspace) {
    auto random = std::mt19937_64{seed};
    const auto schedule = make_repository_schedule(random);
    const auto prepare =
        [&](shared_store_host& host,
            std::optional<liquid::repository>& subject) -> std::optional<std::string> {
        auto opened = host.open();
        if (!opened) {
            return "store open failed: " + message(opened.error());
        }
        auto created = liquid::runtime::block_on(
            liquid::repository::create("workspace/alpha", host.repository_options(3)));
        if (!created) {
            return "repository create failed: " + message(created.error());
        }
        subject.emplace(std::move(*created));
        return std::nullopt;
    };

    auto probe_root = workspace / ("probe-" + std::to_string(seed));
    std::filesystem::remove_all(probe_root);
    auto steps = std::uint64_t{};
    auto state_steps = std::vector<std::uint64_t>{};
    {
        auto host = shared_store_host{kind, probe_root};
        auto subject = std::optional<liquid::repository>{};
        if (auto failed = prepare(host, subject)) {
            return failed;
        }
        auto counter = crash_injector{0};
        auto guard = injector_guard{&counter};
        auto acknowledged = acknowledged_state{};
        auto outcome = run_repository_schedule(host, subject, schedule, acknowledged, counter);
        if (outcome.failure) {
            return "probe " + *outcome.failure;
        }
        steps = counter.steps();
        state_steps = counter.state_steps();
    }
    std::filesystem::remove_all(probe_root);
    if (steps == 0) {
        return "the schedule reached no fault point";
    }

    // Half the seeds crash at a repository-state fault point (LIQUID_REPO, LIQUID_JOURNAL, a
    // replication cursor); the rest anywhere, which is mostly pack, index and lease data.
    const auto target = !state_steps.empty() && random() % 2 == 0
                            ? state_steps[random() % state_steps.size()]
                            : 1 + random() % steps;
    auto actual_root = workspace / ("actual-" + std::to_string(seed));
    std::filesystem::remove_all(actual_root);
    auto host = shared_store_host{kind, actual_root};
    auto subject = std::optional<liquid::repository>{};
    if (auto failed = prepare(host, subject)) {
        return failed;
    }
    auto acknowledged = acknowledged_state{};
    auto crash = crash_injector{target, [&] { host.freeze(true); }};
    auto outcome = schedule_outcome{};
    {
        auto guard = injector_guard{&crash};
        outcome = run_repository_schedule(host, subject, schedule, acknowledged, crash);
    }
    if (outcome.failure) {
        return *outcome.failure;
    }
    // Timestamps inside drips and changes change compressed sizes, so a disk run can roll a pack
    // at a different write and reach a few fault points fewer than its probe. A schedule that ends
    // before its crash point is checked as a clean stop: everything it acknowledged must reopen.
    const auto interrupted = crash.fired() && outcome.interrupted
                                 ? *outcome.interrupted
                                 : scheduled_operation{repository_operation::reopen, {}, {}};
    if (crash.fired() != outcome.interrupted.has_value()) {
        return "the crash at fault point " + std::to_string(target) + " was not reported";
    }
    subject.reset();
    host.objects.reset();
    host.freeze(false);
    auto verified = verify_recovered(host, acknowledged, interrupted);
    host.objects.reset();
    std::filesystem::remove_all(actual_root);
    if (verified) {
        return "after a crash at step " + std::to_string(target) + " of " + std::to_string(steps) +
               " (" + crash.where() + "): " + *verified;
    }
    return std::nullopt;
}

class RepositoryStateSimulation : public testing::TestWithParam<backend_kind> {};

TEST_P(RepositoryStateSimulation, SeededCrashRecovery) {
    const auto* single_seed = std::getenv("LIQUID_REPOSITORY_SIM_SEED");
    const auto first = single_seed ? environment_number("LIQUID_REPOSITORY_SIM_SEED", 0) : 0;
    const auto count = single_seed ? 1 : environment_number("LIQUID_REPOSITORY_SIM_SEEDS", 40);
    const auto workspace =
        liquid::test::fixture_path("liquid-repository-sim-" + backend_name({GetParam(), 0}));
    std::filesystem::create_directories(workspace);
    for (auto offset = std::uint64_t{}; offset != count; ++offset) {
        const auto seed = first + offset;
        const auto failure = run_repository_simulation(seed, GetParam(), workspace);
        ASSERT_FALSE(failure.has_value())
            << "seed=" << seed << ": " << *failure
            << "\nreplay: LIQUID_REPOSITORY_SIM_SEED=" << seed
            << " ctest --test-dir build -R RepositoryStateSimulation --output-on-failure";
    }
    std::filesystem::remove_all(workspace);
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         RepositoryStateSimulation,
                         testing::Values(backend_kind::disk, backend_kind::memory),
                         backend_name);

} // namespace
