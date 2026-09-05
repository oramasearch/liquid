#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "build/events.hpp"
#include "formats/json.hpp"
#include "liquid/build.hpp"
#include "liquid/runtime.hpp"

namespace {

auto next(liquid::build_event_subscription& subscription) -> std::optional<liquid::build_event> {
    auto event = liquid::runtime::block_on(subscription.next());
    EXPECT_TRUE(event) << (event ? "" : event.error().message);
    return event ? std::move(*event) : std::optional<liquid::build_event>{};
}

auto example_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

TEST(BuildEvents, subscribers_receive_the_same_future_events) {
    auto stream = liquid::build_detail::event_access::create();
    auto first = stream.handle.events();
    ASSERT_TRUE(first);

    stream.producer.publish({.kind = liquid::build_event_kind::action_queued});
    auto second = stream.handle.events();
    ASSERT_TRUE(second);
    stream.producer.publish(
        {.kind = liquid::build_event_kind::action_started,
         .execution = liquid::action_execution{liquid::action_execution_kind::local, {}}});
    stream.producer.finish({.actions = 1});

    auto first_queued = next(*first);
    auto first_started = next(*first);
    auto first_finished = next(*first);
    auto second_started = next(*second);
    auto second_finished = next(*second);
    ASSERT_TRUE(first_queued);
    ASSERT_TRUE(first_started);
    ASSERT_TRUE(first_finished);
    ASSERT_TRUE(second_started);
    ASSERT_TRUE(second_finished);
    EXPECT_EQ(first_queued->kind, liquid::build_event_kind::action_queued);
    EXPECT_EQ(first_queued->sequence, 1U);
    EXPECT_EQ(first_started->kind, second_started->kind);
    EXPECT_EQ(first_started->sequence, second_started->sequence);
    EXPECT_EQ(first_finished->kind, second_finished->kind);
    EXPECT_EQ(first_finished->sequence, second_finished->sequence);
    EXPECT_FALSE(next(*first));
    EXPECT_FALSE(next(*second));
}

TEST(BuildEvents, failure_diagnostic_precedes_the_summary) {
    auto stream = liquid::build_detail::event_access::create();
    auto events = stream.handle.events();
    ASSERT_TRUE(events);
    stream.producer.fail({liquid::errc::io, "compiler failed", "source.cpp"}, {.actions = 1});

    auto failed = next(*events);
    auto finished = next(*events);
    ASSERT_TRUE(failed);
    ASSERT_TRUE(finished);
    ASSERT_EQ(failed->kind, liquid::build_event_kind::action_failed);
    ASSERT_EQ(failed->diagnostics.size(), 1U);
    EXPECT_EQ(failed->diagnostics.front().severity, liquid::frontend_diagnostic_severity::error);
    EXPECT_EQ(failed->diagnostics.front().message, "compiler failed: source.cpp");
    ASSERT_EQ(finished->kind, liquid::build_event_kind::build_finished);
    ASSERT_TRUE(finished->summary);
    EXPECT_EQ(finished->summary->exit, 1);
    EXPECT_EQ(finished->summary->failures, 1U);
}

TEST(BuildEvents, json_lines_have_a_stable_schema) {
    auto queued =
        liquid::event_json({.kind = liquid::build_event_kind::action_queued, .sequence = 7});
    ASSERT_TRUE(queued);
    EXPECT_EQ(*queued,
              R"({"v":1,"sequence":7,"type":"action_queued","action":null,"result":null})");

    auto failed = liquid::event_json(
        {.kind = liquid::build_event_kind::action_failed,
         .sequence = 8,
         .diagnostics = {{.file = "source.cpp",
                          .line = 12,
                          .column = 4,
                          .severity = liquid::frontend_diagnostic_severity::error,
                          .message = "bad \"token\""}}});
    ASSERT_TRUE(failed);
    EXPECT_EQ(
        *failed,
        R"({"v":1,"sequence":8,"type":"action_failed","action":null,"result":null,"diagnostics":[{"file":"source.cpp","line":12,"column":4,"severity":"error","message":"bad \"token\""}]})");
}

TEST(BuildPlan, json_lines_have_a_stable_schema) {
    auto action = liquid::build_plan_action{.action = example_digest(1),
                                            .input = {.command = {"clang", "input.c"},
                                                      .cwd = "src",
                                                      .env = {{"LANG", "C"}},
                                                      .outputs = {"input.o"},
                                                      .inputs = example_digest(2),
                                                      .toolchain = example_digest(3),
                                                      .platform = {"linux", "x86_64"},
                                                      .timeout = std::chrono::seconds{30},
                                                      .remote_eligible = true},
                                            .dependencies = {example_digest(4)},
                                            .state = liquid::cache_status::hit,
                                            .result = liquid::result_id{example_digest(5)}};
    auto json = liquid::plan_json(action);
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_EQ(*json,
              "{\"v\":1,\"action\":\"blake3:0100000000000000000000000000000000000000000000"
              "000000000000000000\",\"dependencies\":[\"blake3:04000000000000000000000000000000"
              "00000000000000000000000000000000\"],\"state\":\"hit\",\"result\":\"blake3:05"
              "00000000000000000000000000000000000000000000000000000000000000\",\"input\":{"
              "\"command\":[\"clang\",\"input.c\"],\"cwd\":\"src\",\"env\":{\"LANG\":\"C\"},"
              "\"outputs\":[\"input.o\"],\"inputs\":\"blake3:020000000000000000000000000000000"
              "0000000000000000000000000000000\",\"toolchain\":\"blake3:030000000000000000"
              "0000000000000000000000000000000000000000000000\",\"platform\":{\"os\":\"linux"
              "\",\"arch\":\"x86_64\"},\"timeout_s\":30,\"remote_eligible\":true}}");
    auto parsed = liquid::formats::json::parse(*json);
    EXPECT_TRUE(parsed) << parsed.error().message;
}

} // namespace
