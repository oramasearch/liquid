// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../temporary_directory.hpp"
#include "liquid/graph.hpp"
#include "liquid/runtime.hpp"

namespace {

using namespace std::chrono_literals;

auto as_bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto as_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void write_file(const std::filesystem::path& path, std::string_view value) {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class graph_directory {
public:
    graph_directory() {
        static auto next = std::atomic_uint64_t{};
        root_ =
            liquid::test::fixture_path("liquid-query-graph-" + std::to_string(next.fetch_add(1)));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "tree");
    }

    ~graph_directory() {
        std::filesystem::remove_all(root_);
    }

    auto state() const -> std::filesystem::path {
        return root_ / "state";
    }

    auto tree() const -> std::filesystem::path {
        return root_ / "tree";
    }

private:
    std::filesystem::path root_;
};

auto compile_query(liquid::query_context& context,
                   std::vector<std::byte> arguments,
                   std::atomic_uint64_t* executions)
    -> liquid::runtime::task<std::vector<std::byte>> {
    ++*executions;
    auto source = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::working_tree_blob, as_text(arguments)});
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto text = as_text(source->bytes);
    if (const auto comment = text.find("//"); comment != std::string::npos) {
        text.erase(comment);
    }
    auto digits = std::string{};
    for (const auto character : text) {
        if (character >= '0' && character <= '9') {
            digits += character;
        }
    }
    co_return as_bytes(digits);
}

auto link_query(liquid::query_context& context,
                std::vector<std::byte> arguments,
                std::atomic_uint64_t* executions) -> liquid::runtime::task<std::vector<std::byte>> {
    ++*executions;
    auto object = co_await context.read(liquid::query_key{"compile", std::move(arguments)});
    if (!object) {
        co_return std::unexpected(object.error());
    }
    auto output = as_bytes("linked:");
    output.insert(output.end(), object->bytes.begin(), object->bytes.end());
    co_return output;
}

auto overlay_query(liquid::query_context& context,
                   std::vector<std::byte>,
                   std::atomic_uint64_t* executions)
    -> liquid::runtime::task<std::vector<std::byte>> {
    ++*executions;
    auto value = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::overlay_buffer, "buffer.cpp"});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return std::move(value->bytes);
}

auto toolchain_query(liquid::query_context& context,
                     std::vector<std::byte>,
                     std::atomic_uint64_t* executions)
    -> liquid::runtime::task<std::vector<std::byte>> {
    ++*executions;
    auto value =
        co_await context.read(liquid::graph_input_key{liquid::graph_input_kind::toolchain, "cxx"});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return std::move(value->bytes);
}

auto delayed_query(liquid::query_context& context,
                   std::vector<std::byte>,
                   std::atomic_uint64_t* executions)
    -> liquid::runtime::task<std::vector<std::byte>> {
    const auto sequence = ++*executions;
    auto first = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::overlay_buffer, "buffer.cpp"});
    if (!first) {
        co_return std::unexpected(first.error());
    }
    auto [delay_error] = co_await liquid::runtime::delay(100ms);
    if (delay_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable, "delay cancelled", "query graph test"});
    }
    auto second = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::overlay_buffer, "buffer.cpp"});
    if (!second) {
        co_return std::unexpected(second.error());
    }
    auto output = std::move(second->bytes);
    output.push_back(static_cast<std::byte>(sequence));
    co_return output;
}

struct first_demand_state {
    std::mutex mutex;
    std::condition_variable condition;
    bool done{};
    liquid::result<liquid::graph_value> value;
};

auto run_first_demand(liquid::graph* subject, std::shared_ptr<first_demand_state> state)
    -> liquid::runtime::task<void> {
    auto value = co_await subject->demand({"delayed", {}});
    {
        auto lock = std::scoped_lock{state->mutex};
        state->value = std::move(value);
        state->done = true;
    }
    state->condition.notify_one();
    co_return liquid::result<void>{};
}

auto registry(std::atomic_uint64_t* compiles,
              std::atomic_uint64_t* links,
              std::atomic_uint64_t* overlays = nullptr,
              std::atomic_uint64_t* toolchains = nullptr,
              std::atomic_uint64_t* delayed = nullptr) -> liquid::query_registry {
    auto output = liquid::query_registry{};
    EXPECT_TRUE(output.add(
        {.name = "compile",
         .compute = [compiles](liquid::query_context& context, std::vector<std::byte> arguments) {
             return compile_query(context, std::move(arguments), compiles);
         }}));
    EXPECT_TRUE(output.add(
        {.name = "link",
         .compute = [links](liquid::query_context& context, std::vector<std::byte> arguments) {
             return link_query(context, std::move(arguments), links);
         }}));
    if (overlays) {
        EXPECT_TRUE(output.add({.name = "overlay",
                                .compute = [overlays](liquid::query_context& context,
                                                      std::vector<std::byte> arguments) {
                                    return overlay_query(context, std::move(arguments), overlays);
                                }}));
    }
    if (toolchains) {
        EXPECT_TRUE(output.add({.name = "toolchain",
                                .compute = [toolchains](liquid::query_context& context,
                                                        std::vector<std::byte> arguments) {
                                    return toolchain_query(
                                        context, std::move(arguments), toolchains);
                                }}));
    }
    if (delayed) {
        EXPECT_TRUE(output.add({.name = "delayed",
                                .compute = [delayed](liquid::query_context& context,
                                                     std::vector<std::byte> arguments) {
                                    return delayed_query(context, std::move(arguments), delayed);
                                }}));
    }
    return output;
}

TEST(QueryGraph, registry_canonicalizes_arguments) {
    auto kinds = liquid::query_registry{};
    auto added =
        kinds.add({.name = "canonical",
                   .encode_arguments =
                       [](std::span<const std::byte> arguments) {
                           auto output = std::vector<std::byte>{arguments.begin(), arguments.end()};
                           std::ranges::sort(output);
                           return liquid::result<std::vector<std::byte>>{std::move(output)};
                       },
                   .compute = [](liquid::query_context&, std::vector<std::byte> arguments)
                       -> liquid::runtime::task<std::vector<std::byte>> { co_return arguments; }});
    ASSERT_TRUE(added);
    const auto key = kinds.key("canonical", as_bytes("cba"));
    ASSERT_TRUE(key);
    EXPECT_EQ(as_text(key->arguments), "abc");
}

TEST(QueryGraph, red_green_validation_uses_early_cutoff) {
    auto directory = graph_directory{};
    write_file(directory.tree() / "unit.cpp", "int answer = 42;\n");
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    auto opened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(opened) << opened.error().message;
    const auto key = liquid::query_key{"link", as_bytes("unit.cpp")};
    auto first = liquid::runtime::block_on(opened->demand(key));
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->durability_tier, liquid::durability::low);
    EXPECT_EQ(compiles, 1U);
    EXPECT_EQ(links, 1U);

    write_file(directory.tree() / "unit.cpp", "int answer = 42; // comment only\n");
    auto marked = opened->mark_changed({liquid::graph_input_kind::working_tree_blob, "unit.cpp"},
                                       liquid::durability::low);
    ASSERT_TRUE(marked);
    auto comment = liquid::runtime::block_on(opened->demand(key));
    ASSERT_TRUE(comment) << comment.error().message;
    EXPECT_EQ(compiles, 2U);
    EXPECT_EQ(links, 1U);
    EXPECT_EQ(liquid::to_string(comment->fingerprint), liquid::to_string(first->fingerprint));

    write_file(directory.tree() / "unit.cpp", "int answer = 43;\n");
    marked = opened->mark_changed({liquid::graph_input_kind::working_tree_blob, "unit.cpp"},
                                  liquid::durability::low);
    ASSERT_TRUE(marked);
    auto changed = liquid::runtime::block_on(opened->demand(key));
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(compiles, 3U);
    EXPECT_EQ(links, 2U);
    EXPECT_NE(liquid::to_string(changed->fingerprint), liquid::to_string(first->fingerprint));

    auto forced = opened->mark_changed(liquid::query_key{"compile", as_bytes("unit.cpp")});
    ASSERT_TRUE(forced);
    ASSERT_TRUE(liquid::runtime::block_on(
        opened->demand(liquid::query_key{"compile", as_bytes("unit.cpp")})));
    EXPECT_EQ(compiles, 4U);
}

TEST(QueryGraph, volatile_invalidation_walks_only_volatile_queries) {
    auto directory = graph_directory{};
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    auto overlays = std::atomic_uint64_t{};
    auto toolchains = std::atomic_uint64_t{};
    auto opened = liquid::runtime::block_on(
        liquid::graph::open(directory.state(),
                            registry(&compiles, &links, &overlays, &toolchains),
                            {.working_tree = directory.tree()}));
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        opened->set_input({liquid::graph_input_kind::overlay_buffer, "buffer.cpp"},
                          as_bytes("first"),
                          liquid::durability::volatile_)));
    ASSERT_TRUE(
        liquid::runtime::block_on(opened->set_input({liquid::graph_input_kind::toolchain, "cxx"},
                                                    as_bytes("toolchain"),
                                                    liquid::durability::high)));
    auto overlay = liquid::runtime::block_on(opened->demand({"overlay", {}}));
    auto toolchain = liquid::runtime::block_on(opened->demand({"toolchain", {}}));
    ASSERT_TRUE(overlay);
    ASSERT_TRUE(toolchain);
    EXPECT_EQ(overlay->durability_tier, liquid::durability::volatile_);
    EXPECT_EQ(toolchain->durability_tier, liquid::durability::high);

    const auto before = opened->statistics();
    ASSERT_TRUE(liquid::runtime::block_on(
        opened->set_input({liquid::graph_input_kind::overlay_buffer, "buffer.cpp"},
                          as_bytes("second"),
                          liquid::durability::volatile_)));
    const auto after = opened->statistics();
    EXPECT_EQ(after.invalidation_walks - before.invalidation_walks, 1U);
    ASSERT_TRUE(liquid::runtime::block_on(opened->demand({"toolchain", {}})));
    EXPECT_EQ(toolchains, 1U);
}

TEST(QueryGraph, second_demand_cancels_and_discards_first_computation) {
    auto directory = graph_directory{};
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    auto delayed = std::atomic_uint64_t{};
    auto opened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links, nullptr, nullptr, &delayed)));
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        opened->set_input({liquid::graph_input_kind::overlay_buffer, "buffer.cpp"},
                          as_bytes("first"),
                          liquid::durability::volatile_)));

    auto state = std::make_shared<first_demand_state>();
    liquid::runtime::spawn(run_first_demand(&*opened, state), [](auto) {});
    while (delayed.load() == 0) {
        std::this_thread::yield();
    }
    auto second = liquid::runtime::block_on(opened->demand({"delayed", {}}));
    ASSERT_TRUE(second) << second.error().message;
    {
        auto lock = std::unique_lock{state->mutex};
        state->condition.wait(lock, [&] { return state->done; });
    }
    EXPECT_FALSE(state->value);
    EXPECT_EQ(opened->statistics().discarded_computations, 1U);
    EXPECT_EQ(delayed, 2U);
}

TEST(QueryGraph, reopen_validates_fingerprints_without_reexecuting) {
    auto directory = graph_directory{};
    write_file(directory.tree() / "unit.cpp", "int value = 1;\n");
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
#if defined(__unix__) || defined(__APPLE__)
    const auto marker = directory.state().parent_path() / "crash-ready";
    const auto child = fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        execl(LIQUID_GRAPH_CRASH_FIXTURE,
              LIQUID_GRAPH_CRASH_FIXTURE,
              directory.state().c_str(),
              directory.tree().c_str(),
              marker.c_str(),
              nullptr);
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    const auto ready = std::filesystem::exists(marker);
    kill(child, SIGKILL);
    auto status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(ready);
#else
    auto opened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(opened->demand({"compile", as_bytes("unit.cpp")})));
#endif
    compiles = 0;
    links = 0;
    const auto key = liquid::query_key{"compile", as_bytes("unit.cpp")};
    auto reopened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->demand(key));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(compiles, 0U);
    EXPECT_EQ(links, 0U);
    EXPECT_EQ(reopened->statistics().fingerprint_reads, 1U);
    EXPECT_EQ(reopened->statistics().hits, 1U);
    auto recorded = liquid::runtime::block_on(reopened->recorded(key));
    ASSERT_TRUE(recorded);
    ASSERT_TRUE(*recorded);
    EXPECT_EQ(liquid::to_string((*recorded)->value.fingerprint),
              liquid::to_string(value->fingerprint));
    ASSERT_EQ((*recorded)->dependencies.size(), 1U);
}

TEST(QueryGraphValidation, stale_value_is_named_audited_and_evicted) {
    auto directory = graph_directory{};
    write_file(directory.tree() / "unit.cpp", "int value = 42;\n");
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    const auto key = liquid::query_key{"compile", as_bytes("unit.cpp")};
    {
        auto opened = liquid::runtime::block_on(liquid::graph::open(
            directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
        ASSERT_TRUE(opened) << opened.error().message;
        auto original = liquid::runtime::block_on(opened->demand(key));
        ASSERT_TRUE(original) << original.error().message;
        ASSERT_TRUE(liquid::runtime::block_on(
            opened->inject_stale_value_for_testing(key, as_bytes("wrong"))));
        auto stale = liquid::runtime::block_on(opened->demand(key));
        ASSERT_TRUE(stale) << stale.error().message;
        EXPECT_EQ(as_text(stale->bytes), "wrong");
        EXPECT_EQ(compiles, 1U);
        EXPECT_EQ(opened->validation_status().backlog, 1U);

        auto report = liquid::runtime::block_on(opened->validate({.rate = 20, .all = true}));
        ASSERT_TRUE(report) << report.error().message;
        ASSERT_EQ(report->records.size(), 1U);
        EXPECT_EQ(report->considered, 1U);
        EXPECT_EQ(report->validated, 1U);
        EXPECT_EQ(report->divergences, 1U);
        EXPECT_EQ(report->records.front().query.kind, "compile");
        ASSERT_EQ(report->records.front().inputs.size(), 1U);
        EXPECT_EQ(
            std::get<liquid::graph_input_key>(report->records.front().inputs.front().key).name,
            "unit.cpp");
        EXPECT_FALSE(report->records.front().agreement);
        EXPECT_NE(liquid::to_string(report->records.front().incremental),
                  liquid::to_string(report->records.front().clean));
        EXPECT_EQ(report->backlog, 0U);
        EXPECT_TRUE(report->last_divergence);
        EXPECT_TRUE(opened->validation_status().last_divergence);

        auto repaired = liquid::runtime::block_on(opened->demand(key));
        ASSERT_TRUE(repaired) << repaired.error().message;
        EXPECT_EQ(as_text(repaired->bytes), "42");
        EXPECT_EQ(compiles, 3U);
    }

    auto reopened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto records = liquid::runtime::block_on(reopened->validation_records());
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records->size(), 1U);
    EXPECT_FALSE(records->front().agreement);
    EXPECT_TRUE(reopened->validation_status().last_divergence);
}

TEST(QueryGraphValidation, full_pass_has_no_divergence) {
    auto directory = graph_directory{};
    write_file(directory.tree() / "unit.cpp", "int value = 42;\n");
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    auto opened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(opened->demand({"link", as_bytes("unit.cpp")})));

    auto report = liquid::runtime::block_on(opened->validate({.rate = 20, .all = true}));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->considered, 2U);
    EXPECT_EQ(report->validated, 2U);
    EXPECT_EQ(report->divergences, 0U);
    EXPECT_EQ(report->backlog, 0U);
    EXPECT_FALSE(report->last_divergence);
    EXPECT_EQ(opened->validation_status().validated, 2U);
}

TEST(QueryGraphValidation, default_rate_selects_one_in_twenty_targets) {
    auto directory = graph_directory{};
    auto compiles = std::atomic_uint64_t{};
    auto links = std::atomic_uint64_t{};
    auto opened = liquid::runtime::block_on(liquid::graph::open(
        directory.state(), registry(&compiles, &links), {.working_tree = directory.tree()}));
    ASSERT_TRUE(opened) << opened.error().message;
    for (auto index = 0; index != 21; ++index) {
        const auto name = "unit-" + std::to_string(index) + ".cpp";
        write_file(directory.tree() / name, "int value = " + std::to_string(index) + ";\n");
        ASSERT_TRUE(liquid::runtime::block_on(opened->demand({"compile", as_bytes(name)})));
    }

    auto report = liquid::runtime::block_on(opened->validate());
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->considered, 2U);
    EXPECT_EQ(report->validated, 2U);
    EXPECT_EQ(report->divergences, 0U);
    EXPECT_EQ(report->backlog, 19U);
}

} // namespace
