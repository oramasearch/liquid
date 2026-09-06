#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

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
    temporary_repository()
        : path_(liquid::test::fixture_path("liquid-daemon-test-" +
                                           std::to_string(next_.fetch_add(1)))) {
        std::filesystem::remove_all(path_);
        auto created = liquid::runtime::block_on(liquid::repository::create(path_));
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

    [[nodiscard]] auto deliveries() const -> std::uint64_t {
        std::lock_guard lock(mutex_);
        return deliveries_;
    }

private:
    std::string name_;
    std::vector<std::string>& order_;
    mutable std::mutex mutex_;
    std::uint64_t deliveries_{};
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

} // namespace

TEST(Daemon, public_shapes_are_plain_and_opaque) {
    static_assert(sizeof(liquid::daemon) == sizeof(void*));
    static_assert(std::is_aggregate_v<liquid::daemon_options>);
    static_assert(std::is_aggregate_v<liquid::daemon_status>);
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
    EXPECT_NE(json.find("\"pid\":" + std::to_string(status->process_id)), std::string::npos);
    EXPECT_NE(json.find("\"graph_validation_backlog\":0"), std::string::npos);
    EXPECT_NE(json.find("\"last_graph_divergence\":null"), std::string::npos);

    EXPECT_TRUE(liquid::runtime::block_on(attached->shutdown()));
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
    EXPECT_TRUE(liquid::runtime::block_on(fanout.deliver(delivery)));
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
