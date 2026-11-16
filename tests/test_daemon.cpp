// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "daemon/fanout.hpp"
#include "liquid/daemon.hpp"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "temporary_directory.hpp"

namespace {

class temporary_repository {
public:
    explicit temporary_repository(liquid::repository_options options = {})
        : path_(liquid::test::fixture_path("liquid-daemon-test-" +
                                           std::to_string(next_.fetch_add(1)))) {
        std::filesystem::remove_all(path_);
        auto created = liquid::runtime::block_on(liquid::repository::create(path_, options));
        if (!created) {
            throw std::runtime_error(created.error().message);
        }
    }

    ~temporary_repository() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic<std::uint64_t> next_{};
    std::filesystem::path path_;
};

auto fingerprint(std::string_view value) -> liquid::digest {
    auto hashed =
        liquid::hash(liquid::algorithm::blake3,
                     std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()});
    if (!hashed) {
        throw std::runtime_error(hashed.error().message);
    }
    return *hashed;
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

liquid::runtime::task<std::vector<std::byte>> echo_query(liquid::query_context&,
                                                         std::vector<std::byte> arguments) {
    co_return std::move(arguments);
}

class recording_target final : public liquid::daemon_target {
public:
    recording_target(std::string target_name, std::vector<std::string>& order)
        : name_(std::move(target_name)), order_(order) {}

    auto name() const -> std::string override {
        return name_;
    }

    liquid::runtime::task<void> deliver(liquid::daemon_delivery) override {
        std::lock_guard lock(mutex_);
        ++deliveries_;
        order_.push_back(name_);
        co_return {};
    }

    liquid::runtime::task<void> deliver_presence(liquid::presence value) override {
        std::lock_guard lock(mutex_);
        presence_.push_back(std::move(value));
        co_return {};
    }

    liquid::runtime::task<void> deliver_locks(std::vector<liquid::exclusive_lock> values) override {
        std::lock_guard lock(mutex_);
        locks_.push_back(std::move(values));
        co_return {};
    }

    [[nodiscard]] auto deliveries() const -> std::uint64_t {
        std::lock_guard lock(mutex_);
        return deliveries_;
    }

    [[nodiscard]] auto presence_count() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return presence_.size();
    }

    [[nodiscard]] auto lock_update_count() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return locks_.size();
    }

    [[nodiscard]] auto latest_lock_count() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return locks_.empty() ? 0U : locks_.back().size();
    }

private:
    std::string name_;
    std::vector<std::string>& order_;
    mutable std::mutex mutex_;
    std::uint64_t deliveries_{};
    std::vector<liquid::presence> presence_;
    std::vector<std::vector<liquid::exclusive_lock>> locks_;
};

class stalled_target final : public liquid::daemon_target {
public:
    auto name() const -> std::string override {
        return "stalled";
    }

    liquid::runtime::task<void> deliver(liquid::daemon_delivery) override {
        if (stalled_.load(std::memory_order_acquire)) {
            co_return std::unexpected(
                liquid::error{liquid::errc::busy, "consumer is stalled", "daemon flow test"});
        }
        ++deliveries_;
        co_return {};
    }

    auto release() -> void {
        stalled_.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool stalled_{true};
    std::atomic_uint64_t deliveries_{};
};

auto options(std::chrono::milliseconds idle = std::chrono::seconds{5}) -> liquid::daemon_options {
    auto output = liquid::daemon_options{};
    output.idle_timeout = idle;
    output.capture.coalesce_window = std::chrono::milliseconds{5};
    output.capture.reconcile_interval = std::chrono::milliseconds{0};
    output.capture.scan = false;
    return output;
}

auto wait_for_delivery(liquid::daemon& daemon, std::uint64_t before)
    -> liquid::result<liquid::daemon_status> {
    for (auto attempt = 0; attempt != 500; ++attempt) {
        auto current = liquid::runtime::block_on(daemon.status());
        if (!current) {
            return std::unexpected(current.error());
        }
        const auto complete = !current->targets.empty() &&
                              std::ranges::all_of(current->targets, [before](const auto& target) {
                                  return target.deliveries > before && target.behind == 0;
                              });
        if (complete) {
            return current;
        }
        auto waited = liquid::runtime::block_on(
            [](std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
                auto [delay_error] = co_await liquid::runtime::delay(duration);
                if (delay_error) {
                    co_return std::unexpected(liquid::error{
                        liquid::errc::internal, delay_error.message(), "daemon test"});
                }
                co_return {};
            }(std::chrono::milliseconds{10}));
        if (!waited) {
            return std::unexpected(waited.error());
        }
    }
    return std::unexpected(
        liquid::error{liquid::errc::unavailable, "daemon did not deliver a drip", "daemon test"});
}

#ifndef _WIN32

// Sets one environment variable for the life of a test and puts the old value back.
class environment_override final {
public:
    environment_override(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~environment_override() {
        if (previous_) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    environment_override(const environment_override&) = delete;
    auto operator=(const environment_override&) -> environment_override& = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

// A short, private directory to stand in for $XDG_RUNTIME_DIR.
class runtime_root final {
public:
    runtime_root() {
        auto text = std::string{"/tmp/lq-xdg-XXXXXX"};
        if (::mkdtemp(text.data()) == nullptr) {
            throw std::runtime_error(std::string{"mkdtemp: "} + std::strerror(errno));
        }
        path_ = text;
    }

    ~runtime_root() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    runtime_root(const runtime_root&) = delete;
    auto operator=(const runtime_root&) -> runtime_root& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// A listening socket the test owns, standing in for an impostor at the daemon's path. It accepts
// connections and closes them without answering.
class planted_socket final {
public:
    planted_socket(std::filesystem::path path, ::mode_t mode) : path_(std::move(path)) {
        auto address = sockaddr_un{};
        address.sun_family = AF_UNIX;
        const auto text = path_.string();
        if (text.size() >= sizeof(address.sun_path)) {
            throw std::runtime_error("planted socket path is too long: " + text);
        }
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor_ < 0 ||
            ::bind(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
                0 ||
            ::listen(descriptor_, 4) != 0 || ::chmod(path_.c_str(), mode) != 0) {
            throw std::runtime_error("cannot plant socket at " + text + ": " +
                                     std::strerror(errno));
        }
        worker_ = std::thread{[this] {
            while (!stop_.load(std::memory_order_acquire)) {
                auto ready = pollfd{.fd = descriptor_, .events = POLLIN, .revents = 0};
                if (::poll(&ready, 1, 20) > 0) {
                    const auto connection = ::accept(descriptor_, nullptr, nullptr);
                    if (connection >= 0) {
                        ::close(connection);
                    }
                }
            }
        }};
    }

    ~planted_socket() {
        stop_.store(true, std::memory_order_release);
        worker_.join();
        ::close(descriptor_);
        ::unlink(path_.c_str());
    }

    planted_socket(const planted_socket&) = delete;
    auto operator=(const planted_socket&) -> planted_socket& = delete;

private:
    std::filesystem::path path_;
    int descriptor_{-1};
    std::atomic_bool stop_{false};
    std::thread worker_;
};

#endif

} // namespace

TEST(Daemon, public_shapes_are_plain_and_opaque) {
    static_assert(sizeof(liquid::daemon) == sizeof(void*));
    static_assert(std::is_aggregate_v<liquid::daemon_options>);
    static_assert(std::is_aggregate_v<liquid::daemon_status>);
}

TEST(DaemonFanout, presence_reaches_every_local_target_without_advancing_drip_state) {
    temporary_repository fixture;
    auto order = std::vector<std::string>{};
    auto first = std::make_shared<recording_target>("first", order);
    auto second = std::make_shared<recording_target>("second", order);
    auto fanout =
        liquid::daemon_detail::fanout{fixture.path() / "presence-fanout-state", {first, second}};
    ASSERT_TRUE(fanout.load());
    auto delivered = liquid::runtime::block_on(fanout.deliver_presence(
        {.path = "src/value.cpp", .activity = liquid::presence_activity::editing}));
    ASSERT_TRUE(delivered) << delivered.error().message;
    EXPECT_EQ(first->presence_count(), 1U);
    EXPECT_EQ(second->presence_count(), 1U);
    EXPECT_TRUE(fanout.replay_cursor().token.empty());
}

TEST(DaemonFanout, locks_reach_every_local_target_without_advancing_drip_state) {
    temporary_repository fixture;
    auto order = std::vector<std::string>{};
    auto first = std::make_shared<recording_target>("first", order);
    auto second = std::make_shared<recording_target>("second", order);
    auto fanout =
        liquid::daemon_detail::fanout{fixture.path() / "lock-fanout-state", {first, second}};
    ASSERT_TRUE(fanout.load());
    auto locks = std::vector<liquid::exclusive_lock>{
        {.path = "assets/model.bin", .reason = "large binary update"}};
    auto delivered = liquid::runtime::block_on(fanout.deliver_locks(std::move(locks)));
    ASSERT_TRUE(delivered) << delivered.error().message;
    EXPECT_EQ(first->lock_update_count(), 1U);
    EXPECT_EQ(second->lock_update_count(), 1U);
    EXPECT_EQ(first->latest_lock_count(), 1U);
    EXPECT_TRUE(fanout.replay_cursor().token.empty());
}

#ifndef _WIN32

TEST(Daemon, single_instance_attach_and_shutdown) {
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);

    auto second = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, liquid::errc::already_exists);

    auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
    ASSERT_TRUE(attached) << (attached ? "" : attached.error().message);
    auto hosted_status = liquid::runtime::block_on(started->status());
    auto attached_status = liquid::runtime::block_on(attached->status());
    ASSERT_TRUE(hosted_status);
    ASSERT_TRUE(attached_status);
    EXPECT_EQ(hosted_status->process_id, attached_status->process_id);
    EXPECT_EQ(hosted_status->endpoint, attached_status->endpoint);

    EXPECT_TRUE(liquid::runtime::block_on(attached->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
    EXPECT_TRUE(
        std::filesystem::exists(location.path() / ".liquid" / "daemon" / "query-graph" / "ROOT"));
}

TEST(Daemon, lock_and_unlock_publish_the_current_lock_set) {
    auto repository_options = liquid::repository_options{};
    repository_options.locks.rules.push_back({"generated", {"generated/**"}});
    temporary_repository location{repository_options};
    auto order = std::vector<std::string>{};
    auto target = std::make_shared<recording_target>("editor", order);
    auto daemon_options = options();
    daemon_options.targets.push_back(target);
    auto started =
        liquid::runtime::block_on(liquid::daemon::start(location.path(), daemon_options));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);

    auto locked =
        liquid::runtime::block_on(started->lock("generated/archive.zip", "regenerate archive"));
    ASSERT_TRUE(locked) << locked.error().message;
    EXPECT_EQ(target->lock_update_count(), 1U);
    EXPECT_EQ(target->latest_lock_count(), 1U);
    auto repository = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(repository) << repository.error().message;
    auto locks = liquid::runtime::block_on(repository->locks());
    ASSERT_TRUE(locks) << locks.error().message;
    ASSERT_EQ(locks->size(), 1U);
    EXPECT_EQ(locks->front().reason, "regenerate archive");

    auto unlocked = liquid::runtime::block_on(started->unlock("generated/archive.zip"));
    ASSERT_TRUE(unlocked) << unlocked.error().message;
    EXPECT_EQ(target->lock_update_count(), 2U);
    EXPECT_EQ(target->latest_lock_count(), 0U);
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, attribution_query_crosses_the_wire) {
    temporary_repository location;
    auto source = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("notes.txt", bytes("one\ntwo\n"))));
    auto change = liquid::runtime::block_on(source->cut("notes"));
    ASSERT_TRUE(change);
    auto direct = liquid::runtime::block_on(source->attribution("notes.txt", *change));
    ASSERT_TRUE(direct);

    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto remote = liquid::runtime::block_on(started->attribution("notes.txt", *change));
    ASSERT_TRUE(remote) << (remote ? "" : remote.error().message);
    ASSERT_EQ(remote->size(), direct->size());
    for (auto index = std::size_t{}; index < direct->size(); ++index) {
        EXPECT_EQ(remote->at(index).line, direct->at(index).line);
        EXPECT_EQ(remote->at(index).text, direct->at(index).text);
        EXPECT_EQ(remote->at(index).by.taap_id, direct->at(index).by.taap_id);
        EXPECT_EQ(remote->at(index).session, direct->at(index).session);
        EXPECT_EQ(liquid::to_string(remote->at(index).change.value),
                  liquid::to_string(direct->at(index).change.value));
        EXPECT_EQ(remote->at(index).agent, direct->at(index).agent);
    }
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, exits_when_idle_and_refuses_late_attach) {
    temporary_repository location;
    auto started = liquid::runtime::block_on(
        liquid::daemon::start(location.path(), options(std::chrono::milliseconds{75})));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));

    auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
    ASSERT_FALSE(attached);
    EXPECT_EQ(attached.error().code, liquid::errc::unavailable);
}

TEST(Daemon, drip_invalidates_only_changed_paths_and_advances_targets) {
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto initial = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(initial);
    ASSERT_FALSE(initial->targets.empty());
    const auto before = initial->targets.front().deliveries;

    {
        auto output = std::ofstream{location.path() / "source.cpp"};
        output << "int answer = 42;\n";
    }
    auto delivered = wait_for_delivery(*started, before);
    ASSERT_TRUE(delivered) << (delivered ? "" : delivered.error().message);
    ASSERT_EQ(delivered->invalidated_paths.size(), 1);
    EXPECT_EQ(delivered->invalidated_paths.front(), "source.cpp");
    for (const auto& target : delivered->targets) {
        EXPECT_EQ(target.behind, 0);
        EXPECT_GT(target.deliveries, before);
    }

    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, consumer_lag_crosses_high_watermark_and_resumes_below_low_watermark) {
    auto repository_options = liquid::repository_options{};
    repository_options.flow.pause_above = 2;
    repository_options.flow.resume_below = 1;
    temporary_repository location{repository_options};
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);

    ASSERT_TRUE(liquid::runtime::block_on(started->report_consumer_lag("render", 3)));
    auto paused = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(paused);
    EXPECT_TRUE(paused->flow.paused);
    EXPECT_EQ(paused->flow.reason, liquid::flow_pause_reason::consumers);
    EXPECT_EQ(paused->flow.consumer_lag, 3U);
    auto writer = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(writer);
    auto refused = liquid::runtime::block_on(writer->write("paused.txt", bytes("paused")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::busy);
    EXPECT_NE(refused.error().context.find("paused_for=consumers"), std::string::npos);

    ASSERT_TRUE(liquid::runtime::block_on(started->report_consumer_lag("render", 2)));
    auto between = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(between);
    EXPECT_TRUE(between->flow.paused);
    ASSERT_FALSE(between->flow.buckets.empty());
    EXPECT_EQ(between->flow.buckets.back().rejected, 1U);

    ASSERT_TRUE(liquid::runtime::block_on(started->report_consumer_lag("render", 1)));
    auto resumed = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(resumed);
    EXPECT_FALSE(resumed->flow.paused);
    EXPECT_EQ(resumed->flow.reason, liquid::flow_pause_reason::none);
    ASSERT_FALSE(resumed->flow.buckets.empty());
    EXPECT_EQ(resumed->flow.buckets.back().scope, liquid::flow_scope::repository);
    EXPECT_TRUE(liquid::runtime::block_on(writer->write("resumed.txt", bytes("resumed"))));

    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, stalled_target_pauses_above_high_watermark_and_resumes_after_delivery) {
    auto repository_options = liquid::repository_options{};
    repository_options.flow.pause_above = 1;
    repository_options.flow.resume_below = 0;
    temporary_repository location{repository_options};
    auto stalled = std::make_shared<stalled_target>();
    auto daemon_options = options();
    daemon_options.targets.push_back(stalled);
    auto started =
        liquid::runtime::block_on(liquid::daemon::start(location.path(), daemon_options));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);

    {
        auto first = std::ofstream{location.path() / "first.txt"};
        first << "first\n";
    }
    ASSERT_TRUE(liquid::runtime::block_on(
        [](std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
            auto [delay_error] = co_await liquid::runtime::delay(duration);
            if (delay_error) {
                co_return std::unexpected(liquid::error{
                    liquid::errc::internal, delay_error.message(), "daemon flow test"});
            }
            co_return {};
        }(std::chrono::milliseconds{20})));
    {
        auto second = std::ofstream{location.path() / "second.txt"};
        second << "second\n";
    }

    auto paused = liquid::result<liquid::daemon_status>{};
    for (auto attempt = 0; attempt != 500; ++attempt) {
        paused = liquid::runtime::block_on(started->status());
        ASSERT_TRUE(paused);
        if (paused->flow.reason == liquid::flow_pause_reason::consumers &&
            paused->flow.consumer_lag > repository_options.flow.pause_above) {
            break;
        }
        ASSERT_TRUE(liquid::runtime::block_on(
            [](std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
                auto [delay_error] = co_await liquid::runtime::delay(duration);
                if (delay_error) {
                    co_return std::unexpected(liquid::error{
                        liquid::errc::internal, delay_error.message(), "daemon flow test"});
                }
                co_return {};
            }(std::chrono::milliseconds{10})));
    }
    ASSERT_TRUE(paused->flow.paused);
    EXPECT_EQ(paused->flow.reason, liquid::flow_pause_reason::consumers);
    EXPECT_EQ(paused->flow.consumer_lag, 2U);

    stalled->release();
    auto delivered = wait_for_delivery(*started, 0);
    ASSERT_TRUE(delivered) << (delivered ? "" : delivered.error().message);
    EXPECT_FALSE(delivered->flow.paused);
    EXPECT_EQ(delivered->flow.consumer_lag, 0U);
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, idle_capacity_validates_and_reports_graph_divergence) {
    temporary_repository location;
    const auto graph_directory = location.path() / ".liquid" / "daemon" / "query-graph";
    auto initial_registry = liquid::query_registry{};
    ASSERT_TRUE(initial_registry.add({.name = "echo", .compute = echo_query}));
    auto initial = liquid::runtime::block_on(
        liquid::graph::open(graph_directory, std::move(initial_registry)));
    ASSERT_TRUE(initial) << initial.error().message;
    const auto key = liquid::query_key{"echo", bytes("correct")};
    ASSERT_TRUE(liquid::runtime::block_on(initial->demand(key)));
    ASSERT_TRUE(
        liquid::runtime::block_on(initial->inject_stale_value_for_testing(key, bytes("stale"))));
    initial = liquid::graph{};

    auto daemon_options = options();
    daemon_options.graph_validation_interval = std::chrono::milliseconds{1};
    ASSERT_TRUE(daemon_options.graph_queries.add({.name = "echo", .compute = echo_query}));
    auto started =
        liquid::runtime::block_on(liquid::daemon::start(location.path(), daemon_options));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto status = liquid::result<liquid::daemon_status>{};
    for (auto attempt = 0; attempt != 500; ++attempt) {
        status = liquid::runtime::block_on(started->status());
        ASSERT_TRUE(status) << status.error().message;
        if (status->last_graph_divergence) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_TRUE(status->last_graph_divergence);
    EXPECT_EQ(status->last_graph_divergence->query.kind, "echo");
    EXPECT_EQ(status->last_graph_divergence->query.arguments, bytes("correct"));
    EXPECT_FALSE(status->last_graph_divergence->agreement);
    EXPECT_EQ(status->graph_validation_backlog, 0U);

    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, command_starts_on_first_use_and_attaches_to_same_process) {
    temporary_repository location;
    const auto output = location.path() / "status.json";
    auto child_environment = std::string{};
    const auto temporary_root = std::filesystem::temp_directory_path();
    if (temporary_root.is_relative()) {
        const auto absolute_root = std::filesystem::absolute(temporary_root);
        const auto relative_root = std::filesystem::relative(absolute_root, location.path());
        child_environment = "TMPDIR=\"" + relative_root.string() + "\" ";
    }
    const auto command = std::string{"cd \""} + location.path().string() + "\" && " +
                         child_environment + "\"" + LIQUID_CLI + "\" status --json > \"" +
                         output.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()), 0);

    auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
    ASSERT_TRUE(attached) << (attached ? "" : attached.error().message);
    auto status = liquid::runtime::block_on(attached->status());
    ASSERT_TRUE(status);
    auto input = std::ifstream{output};
    const auto json = std::string{std::istreambuf_iterator<char>{input}, {}};
    auto normalized_json = json;
    const auto lag_prefix = std::string{"\"persistence_lag\":"};
    const auto lag_start = normalized_json.find(lag_prefix);
    if (lag_start != std::string::npos) {
        const auto value_start = lag_start + lag_prefix.size();
        const auto value_end = normalized_json.find(',', value_start);
        normalized_json.replace(value_start, value_end - value_start, "0");
    }
    auto fixture_input = std::ifstream{std::filesystem::path{LIQUID_SOURCE_ROOT} /
                                       "tests/fixtures/status-flow.json"};
    auto flow_fixture = std::string{std::istreambuf_iterator<char>{fixture_input}, {}};
    if (!flow_fixture.empty() && flow_fixture.back() == '\n') {
        flow_fixture.pop_back();
    }
    EXPECT_NE(json.find("\"pid\":" + std::to_string(status->process_id)), std::string::npos);
    EXPECT_NE(json.find("\"graph_validation_backlog\":0"), std::string::npos);
    EXPECT_NE(json.find("\"last_graph_divergence\":null"), std::string::npos);
    EXPECT_NE(normalized_json.find("\"flow\":" + flow_fixture), std::string::npos);
    EXPECT_NE(json.find("\"persistence_lag\":"), std::string::npos);
    EXPECT_NE(json.find("\"consumer_lag\":"), std::string::npos);
    EXPECT_NE(json.find("\"buckets\":[{"), std::string::npos);

    EXPECT_TRUE(liquid::runtime::block_on(attached->shutdown()));
}

// liquid#397: the socket must not sit in the shared temporary directory, and neither the socket
// nor its directory may be reachable by another user.
TEST(Daemon, socket_lives_in_a_private_per_user_directory) {
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto status = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(status) << (status ? "" : status.error().message);

    const auto endpoint = std::filesystem::path{status->endpoint};
    const auto directory = endpoint.parent_path();
    auto ignored = std::error_code{};
    EXPECT_NE(std::filesystem::weakly_canonical(directory, ignored),
              std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored));
    struct stat directory_info{};
    ASSERT_EQ(::lstat(directory.c_str(), &directory_info), 0);
    EXPECT_TRUE(S_ISDIR(directory_info.st_mode));
    EXPECT_EQ(directory_info.st_uid, ::geteuid());
    EXPECT_EQ(directory_info.st_mode & 0777, 0700);
    struct stat socket_info{};
    ASSERT_EQ(::lstat(endpoint.c_str(), &socket_info), 0);
    EXPECT_TRUE(S_ISSOCK(socket_info.st_mode));
    EXPECT_EQ(socket_info.st_uid, ::geteuid());
    EXPECT_EQ(socket_info.st_mode & 077, 0);

    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, runtime_directory_follows_xdg_runtime_dir) {
    runtime_root root;
    environment_override runtime_directory{"XDG_RUNTIME_DIR", root.path().string()};
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto status = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(status) << (status ? "" : status.error().message);
    EXPECT_EQ(std::filesystem::path{status->endpoint}.parent_path(),
              root.path() / ("liquid-" + std::to_string(::geteuid())));
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

TEST(Daemon, start_and_attach_refuse_a_directory_other_users_can_reach) {
    runtime_root root;
    environment_override runtime_directory{"XDG_RUNTIME_DIR", root.path().string()};
    const auto shared = root.path() / ("liquid-" + std::to_string(::geteuid()));
    ASSERT_TRUE(std::filesystem::create_directory(shared));
    ASSERT_EQ(::chmod(shared.c_str(), 0777), 0);
    temporary_repository location;

    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    if (started) {
        EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
        EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
    }
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, liquid::errc::unauthorized) << started.error().message;

    auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
    ASSERT_FALSE(attached);
    EXPECT_EQ(attached.error().code, liquid::errc::unauthorized) << attached.error().message;
}

TEST(Daemon, attach_refuses_a_socket_it_did_not_create) {
    runtime_root root;
    environment_override runtime_directory{"XDG_RUNTIME_DIR", root.path().string()};
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto status = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(status) << (status ? "" : status.error().message);
    const auto endpoint = std::filesystem::path{status->endpoint};
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
    ASSERT_FALSE(std::filesystem::exists(endpoint));

    {
        // Anyone could have created a socket that any user may write to.
        planted_socket impostor{endpoint, 0666};
        auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
        ASSERT_FALSE(attached);
        EXPECT_EQ(attached.error().code, liquid::errc::unauthorized) << attached.error().message;
    }
    {
        // A symbolic link at the daemon's path is never followed.
        const auto elsewhere = root.path() / "elsewhere.sock";
        planted_socket impostor{elsewhere, 0600};
        std::filesystem::create_symlink(elsewhere, endpoint);
        auto attached = liquid::runtime::block_on(liquid::daemon::attach(location.path()));
        std::filesystem::remove(endpoint);
        ASSERT_FALSE(attached);
        EXPECT_EQ(attached.error().code, liquid::errc::unauthorized) << attached.error().message;
    }
    {
        // A regular file left at the path is not replaced by start, either.
        {
            std::ofstream{endpoint} << "not a socket";
        }
        auto restarted =
            liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
        if (restarted) {
            EXPECT_TRUE(liquid::runtime::block_on(restarted->shutdown()));
            EXPECT_TRUE(liquid::runtime::block_on(restarted->wait()));
        }
        ASSERT_FALSE(restarted);
        EXPECT_EQ(restarted.error().code, liquid::errc::unauthorized) << restarted.error().message;
        EXPECT_TRUE(std::filesystem::is_regular_file(endpoint));
    }
}

// A process running as another user that reaches the socket anyway (here because the test opens
// the directory and socket modes) is refused by its peer credentials, and the daemon keeps
// serving. Needs root to switch user.
TEST(Daemon, refuses_a_connection_from_another_uid) {
    if (::geteuid() != 0) {
        GTEST_SKIP() << "needs root to connect as another user";
    }
    runtime_root root;
    environment_override runtime_directory{"XDG_RUNTIME_DIR", root.path().string()};
    temporary_repository location;
    auto started = liquid::runtime::block_on(liquid::daemon::start(location.path(), options()));
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
    auto status = liquid::runtime::block_on(started->status());
    ASSERT_TRUE(status) << (status ? "" : status.error().message);
    const auto endpoint = std::filesystem::path{status->endpoint};
    const auto directory = endpoint.parent_path();
    ASSERT_EQ(::chmod(root.path().c_str(), 0711), 0);
    ASSERT_EQ(::chmod(directory.c_str(), 0711), 0);
    ASSERT_EQ(::chmod(endpoint.c_str(), 0666), 0);

    auto address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.native().size() + 1);
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // Only async-signal-safe calls between fork and _exit.
        ::signal(SIGPIPE, SIG_IGN);
        if (::setgid(65534) != 0 || ::setuid(65534) != 0) {
            ::_exit(2);
        }
        const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor < 0 ||
            ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
                0) {
            ::_exit(3);
        }
        const unsigned char empty_frame[5] = {0, 0, 0, 0, 0};
        static_cast<void>(::write(descriptor, empty_frame, sizeof(empty_frame)));
        unsigned char reply = 0;
        const auto received = ::read(descriptor, &reply, 1);
        ::_exit(received > 0 ? 1 : 0);
    }
    auto child_status = 0;
    ASSERT_EQ(::waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    EXPECT_EQ(WEXITSTATUS(child_status), 0) << "1 means the daemon answered another user";

    ASSERT_EQ(::chmod(endpoint.c_str(), 0600), 0);
    ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
    auto still_serving = liquid::runtime::block_on(started->status());
    EXPECT_TRUE(still_serving) << (still_serving ? "" : still_serving.error().message);
    EXPECT_TRUE(liquid::runtime::block_on(started->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));
}

#else

TEST(Daemon, windows_reports_missing_named_pipe_transport) {
    auto started = liquid::runtime::block_on(liquid::daemon::start("."));
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, liquid::errc::unsupported);
}

#endif

TEST(DaemonFanout, ordered_targets_advance_once_for_duplicate_delivery) {
    const auto directory = liquid::test::fixture_path("liquid-daemon-fanout-test");
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto order = std::vector<std::string>{};
    auto first = std::make_shared<recording_target>("first", order);
    auto second = std::make_shared<recording_target>("second", order);
    auto targets = std::vector<std::shared_ptr<liquid::daemon_target>>{first, second};
    auto fanout = liquid::daemon_detail::fanout{directory / "state.cbor", targets};
    ASSERT_TRUE(fanout.load());
    const auto id = fingerprint("drip");
    auto delivery =
        liquid::daemon_delivery{.value = {.id = {id}, .ops = {{"a.cpp"}}}, .cursor = {"42:0"}};
    ASSERT_TRUE(fanout.observe(delivery.cursor));
    auto pending = fanout.target_status();
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending.front().behind, 1U);
    EXPECT_EQ(pending.back().behind, 1U);
    EXPECT_TRUE(liquid::runtime::block_on(fanout.deliver(delivery)));
    auto delivered = fanout.target_status();
    ASSERT_EQ(delivered.size(), 2U);
    EXPECT_EQ(delivered.front().behind, 0U);
    EXPECT_EQ(delivered.back().behind, 0U);
    EXPECT_TRUE(liquid::runtime::block_on(fanout.deliver(delivery)));
    EXPECT_EQ(order, (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(first->deliveries(), 1);
    EXPECT_EQ(second->deliveries(), 1);

    auto reopened = liquid::daemon_detail::fanout{directory / "state.cbor", targets};
    ASSERT_TRUE(reopened.load());
    EXPECT_TRUE(liquid::runtime::block_on(reopened.deliver(delivery)));
    EXPECT_EQ(first->deliveries(), 1);
    EXPECT_EQ(second->deliveries(), 1);
    std::filesystem::remove_all(directory);
}

TEST(DaemonGraph, least_recently_demanded_eviction_keeps_metadata_and_persists) {
    const auto directory = liquid::test::fixture_path("liquid-daemon-graph-test");
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto first_fingerprint = liquid::digest{};
    auto second_fingerprint = liquid::digest{};
    {
        auto registry = liquid::query_registry{};
        ASSERT_TRUE(registry.add({.name = "echo", .compute = echo_query}));
        auto opened = liquid::runtime::block_on(
            liquid::graph::open(directory, std::move(registry), {.memory_ceiling = 4}));
        ASSERT_TRUE(opened);
        auto first = liquid::runtime::block_on(opened->demand({"echo", bytes("1111")}));
        auto second = liquid::runtime::block_on(opened->demand({"echo", bytes("2222")}));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        first_fingerprint = first->fingerprint;
        second_fingerprint = second->fingerprint;
        const auto status = opened->statistics();
        EXPECT_EQ(status.keys, 2);
        EXPECT_EQ(status.resident_values, 1);
        EXPECT_EQ(status.resident_value_bytes, 4);
        EXPECT_EQ(status.memory_ceiling, 4);
        auto first_recorded = liquid::runtime::block_on(opened->recorded({"echo", bytes("1111")}));
        auto second_recorded = liquid::runtime::block_on(opened->recorded({"echo", bytes("2222")}));
        ASSERT_TRUE(first_recorded);
        ASSERT_TRUE(second_recorded);
        ASSERT_TRUE(*first_recorded);
        ASSERT_TRUE(*second_recorded);
        EXPECT_EQ(liquid::to_string((*first_recorded)->fingerprint),
                  liquid::to_string(first_fingerprint));
        EXPECT_EQ(liquid::to_string((*second_recorded)->fingerprint),
                  liquid::to_string(second_fingerprint));
        ASSERT_TRUE(liquid::runtime::block_on(opened->flush()));
    }

    auto registry = liquid::query_registry{};
    ASSERT_TRUE(registry.add({.name = "echo", .compute = echo_query}));
    auto reopened = liquid::runtime::block_on(
        liquid::graph::open(directory, std::move(registry), {.memory_ceiling = 4}));
    ASSERT_TRUE(reopened);
    const auto reopened_status = reopened->statistics();
    EXPECT_EQ(reopened_status.keys, 2);
    EXPECT_EQ(reopened_status.resident_values, 0);
    auto first_recorded = liquid::runtime::block_on(reopened->recorded({"echo", bytes("1111")}));
    auto second_recorded = liquid::runtime::block_on(reopened->recorded({"echo", bytes("2222")}));
    ASSERT_TRUE(first_recorded);
    ASSERT_TRUE(second_recorded);
    ASSERT_TRUE(*first_recorded);
    ASSERT_TRUE(*second_recorded);
    EXPECT_EQ(liquid::to_string((*first_recorded)->fingerprint),
              liquid::to_string(first_fingerprint));
    EXPECT_EQ(liquid::to_string((*second_recorded)->fingerprint),
              liquid::to_string(second_fingerprint));
    std::filesystem::remove_all(directory);
}
