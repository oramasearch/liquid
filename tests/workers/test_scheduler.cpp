#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "workers/protocol.hpp"
#include "workers/scheduler.hpp"

namespace {

using namespace std::chrono_literals;

auto decoded_action(std::string name) -> liquid::reapi::DecodedAction {
    return {.input = {.command = {std::move(name)},
                      .inputs = {.alg = liquid::algorithm::blake3},
                      .toolchain = {.alg = liquid::algorithm::blake3},
                      .platform = {"linux", "x86_64"}},
            .sandbox = {.alg = liquid::algorithm::blake3}};
}

auto action_digest(std::string hash) -> liquid::reapi::Digest {
    return {.hash = std::move(hash), .size_bytes = 1};
}

auto schedule(liquid::action_size_class size, std::string reason) -> liquid::execution_schedule {
    return {.decision = liquid::execution_decision::remote,
            .inputs = {.history = {{.exec = 3200ms}, {.exec = 3400ms}, {.exec = 3600ms}},
                       .size_class = size,
                       .input_bytes = 1024,
                       .remote_state_queried = true,
                       .remote_cache_locality_permille = 1000},
            .local_estimate = 3400ms,
            .remote_estimate = 2700ms,
            .reason = std::move(reason)};
}

auto request(std::string worker, liquid::action_size_class size)
    -> liquid::workers::worker_request {
    return {.worker = std::move(worker), .platform = {"linux", "x86_64"}, .size_class = size};
}

TEST(WorkerProtocol, schedule_round_trips_with_a_lease) {
    const auto original = liquid::workers::lease_response{
        .assigned = true,
        .operation = "operations/liquid-7",
        .token = 42,
        .retry = 1,
        .action_digest = action_digest("action"),
        .schedule = schedule(liquid::action_size_class::long_action, "history selected remote"),
        .native_action = {.alg = liquid::algorithm::blake3},
        .remote_eligible = true};
    auto encoded = liquid::workers::encode(original);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::workers::decode<liquid::workers::lease_response>(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(decoded->assigned);
    EXPECT_EQ(decoded->operation, original.operation);
    EXPECT_EQ(decoded->token, original.token);
    EXPECT_EQ(decoded->retry, original.retry);
    EXPECT_EQ(decoded->schedule.inputs.size_class, original.schedule.inputs.size_class);
    EXPECT_EQ(decoded->schedule.inputs.history.size(), 3U);
    EXPECT_EQ(decoded->schedule.reason, original.schedule.reason);
    EXPECT_EQ(decoded->native_action.alg, original.native_action.alg);
    EXPECT_EQ(decoded->native_action.bytes, original.native_action.bytes);
    EXPECT_TRUE(decoded->remote_eligible);
}

TEST(WorkerProtocol, idle_lease_omits_assignment_fields) {
    auto encoded = liquid::workers::encode(liquid::workers::lease_response{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::workers::decode<liquid::workers::lease_response>(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_FALSE(decoded->assigned);
    EXPECT_TRUE(decoded->operation.empty());
}

TEST(WorkerScheduler, duplicate_submission_joins_the_existing_operation) {
    auto scheduler = liquid::workers::scheduler{1s};
    auto first = scheduler.submit(action_digest("same"),
                                  liquid::reapi::DigestFunctionValue::blake3,
                                  {},
                                  {},
                                  decoded_action("first"),
                                  {.alg = liquid::algorithm::blake3},
                                  schedule(liquid::action_size_class::medium, "first"));
    auto second = scheduler.submit(action_digest("same"),
                                   liquid::reapi::DigestFunctionValue::blake3,
                                   {},
                                   {},
                                   decoded_action("second"),
                                   {.alg = liquid::algorithm::blake3},
                                   schedule(liquid::action_size_class::long_action, "second"));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first->name, second->name);
    EXPECT_EQ(scheduler.status().queued, 1U);
}

TEST(WorkerScheduler, placement_honours_the_recorded_size_class) {
    auto scheduler = liquid::workers::scheduler{1s};
    ASSERT_TRUE(
        scheduler.submit(action_digest("short"),
                         liquid::reapi::DigestFunctionValue::blake3,
                         {},
                         {},
                         decoded_action("short"),
                         {.alg = liquid::algorithm::blake3},
                         schedule(liquid::action_size_class::short_action, "short history")));
    ASSERT_TRUE(scheduler.submit(action_digest("long"),
                                 liquid::reapi::DigestFunctionValue::blake3,
                                 {},
                                 {},
                                 decoded_action("long"),
                                 {.alg = liquid::algorithm::blake3},
                                 schedule(liquid::action_size_class::long_action, "long history")));
    auto short_lease =
        scheduler.lease(request("short-worker", liquid::action_size_class::short_action));
    ASSERT_TRUE(short_lease) << short_lease.error().message;
    ASSERT_TRUE(short_lease->assigned);
    EXPECT_EQ(short_lease->action_digest.hash, "short");
    EXPECT_EQ(short_lease->schedule.reason, "short history");
    auto long_lease =
        scheduler.lease(request("long-worker", liquid::action_size_class::long_action));
    ASSERT_TRUE(long_lease) << long_lease.error().message;
    ASSERT_TRUE(long_lease->assigned);
    EXPECT_EQ(long_lease->action_digest.hash, "long");
    EXPECT_EQ(long_lease->schedule.reason, "long history");
}

TEST(WorkerScheduler, an_expired_worker_requeues_once) {
    auto scheduler = liquid::workers::scheduler{1ms};
    auto submitted = scheduler.submit(action_digest("retry"),
                                      liquid::reapi::DigestFunctionValue::blake3,
                                      {},
                                      {},
                                      decoded_action("retry"),
                                      {.alg = liquid::algorithm::blake3},
                                      schedule(liquid::action_size_class::medium, "retry history"));
    ASSERT_TRUE(submitted) << submitted.error().message;
    auto first = scheduler.lease(request("worker-one", liquid::action_size_class::medium));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first->assigned);
    std::this_thread::sleep_for(5ms);
    auto second = scheduler.lease(request("worker-two", liquid::action_size_class::medium));
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(second->assigned);
    EXPECT_EQ(second->retry, 1U);
    EXPECT_NE(second->token, first->token);
    EXPECT_EQ(scheduler.status().requeued, 1U);
    std::this_thread::sleep_for(5ms);
    const auto status = scheduler.status();
    EXPECT_EQ(status.requeued, 1U);
    EXPECT_EQ(status.failed, 1U);
    auto operation = scheduler.operation(submitted->name);
    ASSERT_TRUE(operation) << operation.error().message;
    EXPECT_TRUE(operation->done);
}

} // namespace
