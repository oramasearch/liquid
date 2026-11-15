// liquid #526: several processes write one store without an owner (a checkout's daemon, the CLI
// and a library client such as an IDE). Every mutation takes the store's cross-process write
// lock, a handle reloads what another process published before it writes, temporary names are
// unique per writer, and an index that names a missing run is rebuilt from the packs.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/daemon.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "owner_fixture_content.hpp"
#include "store/backend.hpp"
#include "store/pack.hpp"

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path fresh(const std::string& name) {
    const auto path = liquid::test::fixture_path(name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

std::string file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Every file under `directory` with its size and contents, for comparing a store before and after.
std::map<std::string, std::string> snapshot(const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.emplace(entry.path().lexically_relative(directory).generic_string(),
                          file_text(entry.path()));
        }
    }
    return files;
}

// Every frame of every pack decodes and hashes to the digest its pack table records.
std::size_t check_packs(const std::filesystem::path& root) {
    auto backend = liquid::make_disk_store_backend(root);
    auto packs = liquid::runtime::block_on(backend->list("packs/pack-"));
    EXPECT_TRUE(packs) << packs.error().message;
    if (!packs)
        return 0;
    std::size_t frames = 0;
    for (const auto& file : *packs) {
        const auto name = std::filesystem::path(file.key).filename().string();
        if (!name.ends_with(".pack"))
            continue;
        const auto id = static_cast<std::uint32_t>(std::stoul(name.substr(5, name.size() - 10)));
        auto pack =
            liquid::runtime::block_on(liquid::store_detail::read_pack(backend, file.key, id));
        EXPECT_TRUE(pack) << file.key << ": " << pack.error().message;
        if (!pack)
            continue;
        for (const auto& entry : pack->entries) {
            auto bytes = liquid::runtime::block_on(
                liquid::store_detail::read_pack_entry(backend, file.key, entry.location, entry.id));
            EXPECT_TRUE(bytes) << file.key << " frame at " << entry.location.offset << ": "
                               << bytes.error().message;
            ++frames;
        }
    }
    return frames;
}

liquid::store_options small_packs() {
    liquid::store_options options;
    options.pack_size = 64 * 1024;
    return options;
}

TEST(StoreWriters, ManifestNamingAMissingRunIsRebuiltFromThePacks) {
    const auto root = fresh("store-writers-missing-run") / "store";
    std::vector<liquid::digest> written;
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
        ASSERT_TRUE(store) << store.error().message;
        for (std::uint64_t index = 0; index != 24; ++index) {
            auto put =
                liquid::runtime::block_on(store->put(liquid::test::owner_fixture_bytes(7, index)));
            ASSERT_TRUE(put) << put.error().message;
            written.push_back(*put);
        }
    }
    // The packs sealed, so the index has runs. Remove the newest one the manifest names.
    std::vector<std::filesystem::path> runs;
    for (const auto& entry : std::filesystem::directory_iterator(root / "index")) {
        if (entry.path().extension() == ".idx")
            runs.push_back(entry.path());
    }
    ASSERT_FALSE(runs.empty());
    std::filesystem::remove(runs.back());

    auto store = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
    ASSERT_TRUE(store) << store.error().message;
    const auto recoveries = store->recoveries();
    ASSERT_EQ(recoveries.size(), 1U);
    EXPECT_TRUE(recoveries.front().starts_with("index rebuilt from packs: ")) << recoveries.front();
    // A later process, such as the CLI after the daemon repaired the store, can read it too.
    EXPECT_NE(file_text(root / "recoveries.log").find("index rebuilt from packs: "),
              std::string::npos);
    for (std::uint64_t index = 0; index != written.size(); ++index) {
        auto read = liquid::runtime::block_on(store->get(written[index]));
        ASSERT_TRUE(read) << read.error().message;
        EXPECT_EQ(*read, liquid::test::owner_fixture_bytes(7, index));
    }
    // The rebuilt index is whole: the next open repairs nothing.
    auto closed = liquid::runtime::block_on(store->close());
    ASSERT_TRUE(closed);
    auto again = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again->recoveries().empty());
}

TEST(StoreWriters, OpeningAStoreWritesNothing) {
    const auto root = fresh("store-writers-open-reads") / "store";
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
        ASSERT_TRUE(store) << store.error().message;
        for (std::uint64_t index = 0; index != 12; ++index) {
            auto put =
                liquid::runtime::block_on(store->put(liquid::test::owner_fixture_bytes(3, index)));
            ASSERT_TRUE(put) << put.error().message;
        }
    }
    {
        auto store = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
        ASSERT_TRUE(store) << store.error().message;
    }
    const auto before = snapshot(root);
    for (auto round = 0; round != 3; ++round) {
        auto store = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
        ASSERT_TRUE(store) << store.error().message;
        EXPECT_TRUE(store->recoveries().empty());
    }
    // Every open used to write a full copy of the index as a new run and rewrite the manifest,
    // which made every reader a writer that raced the real writers.
    auto after = snapshot(root);
    // The lock file records the last holder's pid and start time; only its generation counts.
    ASSERT_TRUE(before.contains("store.write.lock"));
    EXPECT_EQ(before.at("store.write.lock").substr(0, 8),
              after.at("store.write.lock").substr(0, 8));
    auto without_lock = [](std::map<std::string, std::string> files) {
        files.erase("store.write.lock");
        return files;
    };
    EXPECT_EQ(without_lock(before), without_lock(after));
}

TEST(StoreWriters, ASecondHandleSeesWhatTheFirstPublished) {
    const auto root = fresh("store-writers-two-handles") / "store";
    auto first = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
    ASSERT_TRUE(first) << first.error().message;
    auto second = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
    ASSERT_TRUE(second) << second.error().message;

    // Each handle writes while the other's in-memory view is stale. Without the write lock both
    // append at the same pack offset and the later object map drops the earlier objects.
    std::vector<std::pair<std::uint64_t, liquid::digest>> written;
    for (std::uint64_t index = 0; index != 20; ++index) {
        auto& writer = index % 2 == 0 ? *first : *second;
        auto put =
            liquid::runtime::block_on(writer.put(liquid::test::owner_fixture_bytes(5, index)));
        ASSERT_TRUE(put) << put.error().message;
        written.emplace_back(index, *put);
    }
    for (auto* reader : {&*first, &*second}) {
        for (const auto& [index, id] : written) {
            auto read = liquid::runtime::block_on(reader->get(id));
            ASSERT_TRUE(read) << "index " << index << ": " << read.error().message;
            EXPECT_EQ(*read, liquid::test::owner_fixture_bytes(5, index));
        }
    }
    EXPECT_GT(check_packs(root), 0U);
    auto reopened_first = liquid::runtime::block_on(first->close());
    ASSERT_TRUE(reopened_first);
    auto reopened_second = liquid::runtime::block_on(second->close());
    ASSERT_TRUE(reopened_second);
    auto reopened = liquid::runtime::block_on(liquid::store::open(root, small_packs()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(reopened->recoveries().empty());
    for (const auto& [index, id] : written) {
        auto read = liquid::runtime::block_on(reopened->get(id));
        ASSERT_TRUE(read) << "index " << index << ": " << read.error().message;
    }
}

#ifndef _WIN32

struct child {
    pid_t pid{-1};
    std::filesystem::path log;
};

child spawn(std::vector<std::string> arguments,
            const std::filesystem::path& directory,
            const std::filesystem::path& log) {
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto pid = ::fork();
    if (pid == 0) {
        if (!directory.empty() && ::chdir(directory.c_str()) != 0)
            std::_Exit(126);
        const auto output = ::open(log.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
        if (output >= 0) {
            ::dup2(output, STDOUT_FILENO);
            ::dup2(output, STDERR_FILENO);
        }
        ::execv(argv.front(), argv.data());
        std::_Exit(127);
    }
    return child{pid, log};
}

int wait_for(const child& value) {
    int status = 0;
    if (::waitpid(value.pid, &status, 0) != value.pid)
        return -1;
    return status;
}

bool exited_cleanly(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::map<std::uint64_t, liquid::digest> acknowledged(const std::filesystem::path& ack) {
    std::map<std::uint64_t, liquid::digest> output;
    const auto text = file_text(ack);
    std::size_t start = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
        const auto line = text.substr(start, end - start);
        start = end + 1;
        const auto space = line.find(' ');
        if (space == std::string::npos)
            continue;
        auto parsed = liquid::parse_digest(line.substr(space + 1));
        if (parsed)
            output.emplace(std::stoull(line.substr(0, space)), *parsed);
    }
    return output;
}

TEST(StoreWriters, UnownedWriterProcessesAndACollectorKeepEveryWriteAndThePacksWhole) {
    const auto workspace = fresh("store-writers-processes");
    const auto root = workspace / "store";
    const auto stop = workspace / "stop";
    constexpr std::uint64_t writes = 48;
    std::vector<child> writers;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        writers.push_back(spawn({LIQUID_STORE_WRITERS_FIXTURE,
                                 "store",
                                 root.string(),
                                 (workspace / ("ack-" + std::to_string(seed))).string(),
                                 std::to_string(seed),
                                 std::to_string(writes),
                                 std::to_string(128 * 1024),
                                 workspace.string()},
                                {},
                                workspace / ("log-" + std::to_string(seed))));
    }
    const auto collector =
        spawn({LIQUID_STORE_WRITERS_FIXTURE, "collect", root.string(), stop.string()},
              {},
              workspace / "log-collector");
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        const auto status = wait_for(writers[seed - 1]);
        EXPECT_TRUE(exited_cleanly(status))
            << "writer " << seed << " status " << status << ": "
            << file_text(workspace / ("log-" + std::to_string(seed)));
        EXPECT_EQ(acknowledged(workspace / ("ack-" + std::to_string(seed))).size(), writes);
    }
    std::ofstream(stop) << "stop\n";
    const auto collector_status = wait_for(collector);
    EXPECT_TRUE(exited_cleanly(collector_status)) << "collector status " << collector_status << ": "
                                                  << file_text(workspace / "log-collector");

    EXPECT_GT(check_packs(root), 0U);
    auto store = liquid::runtime::block_on(liquid::store::open(root));
    ASSERT_TRUE(store) << store.error().message;
    EXPECT_TRUE(store->recoveries().empty());
    std::size_t checked = 0;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        for (const auto& [index, id] : acknowledged(workspace / ("ack-" + std::to_string(seed)))) {
            auto read = liquid::runtime::block_on(store->get(id));
            ASSERT_TRUE(read) << "seed " << seed << " index " << index << ": "
                              << read.error().message;
            ASSERT_EQ(*read, liquid::test::owner_fixture_bytes(seed, index));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 4 * writes);
    // The writers' leases are still live, so a full collection keeps every acknowledged object.
    auto collected = liquid::runtime::block_on(store->collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_EQ(collected->live_objects, checked);
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        for (const auto& [index, id] : acknowledged(workspace / ("ack-" + std::to_string(seed)))) {
            auto read = liquid::runtime::block_on(store->get(id));
            ASSERT_TRUE(read) << read.error().message;
        }
    }
}

#ifdef LIQUID_CLI

// The CLI, at `directory`, with its output in `log`; returns the exit status.
int liquid_cli(const std::filesystem::path& directory,
               const std::filesystem::path& log,
               std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), LIQUID_CLI);
    return wait_for(spawn(std::move(arguments), directory, log));
}

std::chrono::seconds stress_duration() {
    if (const auto* text = std::getenv("LIQUID_STORE_STRESS_SECONDS")) {
        return std::chrono::seconds{std::strtoll(text, nullptr, 10)};
    }
    return std::chrono::seconds{60};
}

// The reproduction of liquid #526: the checkout daemon, the CLI and a library client write one
// checkout's store at once for a minute. Afterwards the store opens without repair, every pack
// frame verifies, the CLI still works, and the repository opens and walks its reachable graph.
TEST(StoreWriters, DaemonCliAndLibraryClientWriteOneCheckoutTogether) {
    const auto workspace = fresh("store-writers-checkout");
    const auto checkout = workspace / "checkout";
    const auto logs = workspace / "logs";
    std::filesystem::create_directories(checkout / "notes");
    std::filesystem::create_directories(logs);
    ::setenv("LIQUID_HOME", (workspace / "identity").c_str(), 1);
    std::ofstream(checkout / "a.txt") << "a\n";
    std::ofstream(checkout / "b.txt") << "b\n";
    std::ofstream(checkout / "notes" / "c.txt") << "c\n";
    ASSERT_TRUE(exited_cleanly(liquid_cli(checkout, logs / "init", {"init", "."})))
        << file_text(logs / "init");

    const auto daemon =
        spawn({LIQUID_CLI, "daemon", "--serve", checkout.string(), "--idle-ms", "600000"},
              checkout,
              logs / "daemon");
    auto attached = false;
    for (auto attempt = 0; attempt != 500 && !attached; ++attempt) {
        attached = liquid::runtime::block_on(liquid::daemon::attach(checkout)).has_value();
        if (!attached)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_TRUE(attached) << file_text(logs / "daemon");

    const auto stop = workspace / "stop";
    const auto library = spawn({LIQUID_STORE_WRITERS_FIXTURE,
                                "repository",
                                checkout.string(),
                                (workspace / "library-ack").string(),
                                stop.string()},
                               {},
                               logs / "library");

    const auto deadline = std::chrono::steady_clock::now() + stress_duration();
    std::uint64_t round = 0;
    std::vector<std::string> failures;
    while (std::chrono::steady_clock::now() < deadline) {
        // An edit the daemon captures, then the CLI reading and cutting beside it.
        std::ofstream(checkout / (round % 2 == 0 ? "a.txt" : "notes/c.txt"), std::ios::app)
            << "round " << round << "\n";
        const auto status_log = logs / ("status-" + std::to_string(round));
        if (!exited_cleanly(liquid_cli(checkout, status_log, {"status"})))
            failures.push_back("status: " + file_text(status_log));
        if (round % 4 == 3) {
            const auto cut_log = logs / ("cut-" + std::to_string(round));
            if (!exited_cleanly(
                    liquid_cli(checkout, cut_log, {"cut", "round " + std::to_string(round)})))
                failures.push_back("cut: " + file_text(cut_log));
        }
        ++round;
    }
    std::ofstream(stop) << "stop\n";
    const auto library_status = wait_for(library);
    EXPECT_TRUE(exited_cleanly(library_status)) << file_text(logs / "library");
    EXPECT_GT(acknowledged(workspace / "library-ack").size(), 0U);
    EXPECT_TRUE(exited_cleanly(liquid_cli(checkout, logs / "final-status", {"status"})))
        << file_text(logs / "final-status");
    // liquid #527: status reports the store's leases.
    EXPECT_NE(file_text(logs / "final-status").find("\nleases="), std::string::npos)
        << file_text(logs / "final-status");
    EXPECT_TRUE(exited_cleanly(liquid_cli(checkout, logs / "shutdown", {"daemon", "--shutdown"})))
        << file_text(logs / "shutdown");
    const auto daemon_status = wait_for(daemon);
    EXPECT_TRUE(exited_cleanly(daemon_status)) << file_text(logs / "daemon");
    for (const auto& failure : failures)
        ADD_FAILURE() << failure;
    EXPECT_GT(round, 4U);

    // The store opens as it is, every frame verifies, and nothing was left half-published.
    const auto store_root = checkout / ".liquid-store";
    EXPECT_GT(check_packs(store_root), 0U);
    {
        auto store = liquid::runtime::block_on(liquid::store::open(store_root));
        ASSERT_TRUE(store) << store.error().message;
        EXPECT_TRUE(store->recoveries().empty());
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(checkout));
    ASSERT_TRUE(repository) << repository.error().message;
    auto library_file = liquid::runtime::block_on(repository->read("library/0.txt"));
    ASSERT_TRUE(library_file) << library_file.error().message;
    // A dry-run collection loads every object reachable from every live lease.
    auto options = liquid::collection_options{};
    options.dry_run = true;
    auto walked = liquid::runtime::block_on(repository->collect(options));
    ASSERT_TRUE(walked) << walked.error().message;
    EXPECT_GT(walked->live_objects, 0U);
}

#endif
#endif

} // namespace
