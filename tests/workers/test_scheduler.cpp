// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "../temporary_directory.hpp"
#include "identity/taap.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"
#include "reapi/conversion.hpp"
#include "workers/auth.hpp"
#include "workers/protocol.hpp"
#include "workers/scheduler.hpp"

namespace {

using namespace std::chrono_literals;

auto text_bytes(std::string_view value) -> std::vector<std::byte> {
    const auto raw = std::as_bytes(std::span{value});
    return {raw.begin(), raw.end()};
}

auto content_digest(std::string_view seed) -> liquid::digest {
    auto value = liquid::hash(liquid::algorithm::blake3, text_bytes(seed));
    if (!value) {
        throw std::runtime_error{value.error().message};
    }
    return *value;
}

// One TAAP identity per name, each created in its own LIQUID_HOME and kept for the process.
auto identity(const std::string& name) -> liquid::signer& {
    static auto identities = std::map<std::string, liquid::signer>{};
    if (const auto found = identities.find(name); found != identities.end()) {
        return found->second;
    }
    const auto home = liquid::test::fixture_path("liquid-worker-auth-" + name);
    auto code = std::error_code{};
    std::filesystem::remove_all(home, code);
    const auto* configured = std::getenv("LIQUID_HOME");
    const auto previous = configured == nullptr ? std::optional<std::string>{}
                                                : std::optional<std::string>{configured};
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto created = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (previous) {
        setenv("LIQUID_HOME", previous->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return identities.emplace(name, std::move(*created)).first->second;
}

auto worker_id(const std::string& name) -> std::string {
    return identity(name).identity().taap_id;
}

template <typename Message>
auto signed_by(liquid::signer& key, std::string_view purpose, Message value) -> Message {
    auto body = liquid::workers::signing_body(value);
    if (!body) {
        throw std::runtime_error{body.error().message};
    }
    auto signed_value = liquid::runtime::block_on(key.sign(std::string{purpose}, std::move(*body)));
    if (!signed_value) {
        throw std::runtime_error{signed_value.error().message};
    }
    auto signature = liquid::identity_detail::canonical_signature(*signed_value);
    if (!signature) {
        throw std::runtime_error{signature.error().message};
    }
    value.signature = std::move(*signature);
    return value;
}

// Stamps a fresh nonce and the current time, then signs with `key`.
template <typename Message>
auto fresh(liquid::signer& key, std::string_view purpose, Message value) -> Message {
    value.nonce = *liquid::workers::random_token();
    value.issued_at_ms = liquid::workers::unix_milliseconds();
    return signed_by(key, purpose, std::move(value));
}

auto decoded_action(std::string name) -> liquid::reapi::DecodedAction {
    return {.input = {.command = {std::move(name)},
                      .inputs = content_digest("inputs"),
                      .toolchain = content_digest("toolchain"),
                      .platform = {"linux", "x86_64"}},
            .sandbox = content_digest("sandbox")};
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

auto unsigned_request(const std::string& worker, liquid::action_size_class size)
    -> liquid::workers::worker_request {
    return {.worker = worker_id(worker), .platform = {"linux", "x86_64"}, .size_class = size};
}

// A Lease request from the named worker, signed by its own key.
auto request(const std::string& worker, liquid::action_size_class size)
    -> liquid::workers::worker_request {
    return fresh(
        identity(worker), liquid::workers::lease_request_purpose, unsigned_request(worker, size));
}

auto trusting(std::initializer_list<std::string> workers) -> std::vector<liquid::actor> {
    auto output = std::vector<liquid::actor>{};
    for (const auto& worker : workers) {
        output.push_back(identity(worker).identity());
    }
    return output;
}

auto submit_one(liquid::workers::scheduler& scheduler, std::string name)
    -> liquid::result<liquid::reapi::Operation> {
    return scheduler.submit(liquid::reapi::Digest{.hash = name, .size_bytes = 1},
                            liquid::reapi::DigestFunctionValue::blake3,
                            {},
                            {},
                            decoded_action(name),
                            {.alg = liquid::algorithm::blake3},
                            schedule(liquid::action_size_class::medium, name));
}

TEST(WorkerProtocol, schedule_round_trips_with_a_lease) {
    const auto original = liquid::workers::lease_response{
        .assigned = true,
        .operation = "operations/liquid-7",
        .token = "00112233445566778899aabbccddeeff",
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
    auto scheduler =
        liquid::workers::scheduler{1s, trusting({"first-worker", "short-worker", "long-worker"})};
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
    auto scheduler =
        liquid::workers::scheduler{1s, trusting({"first-worker", "short-worker", "long-worker"})};
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
    auto scheduler = liquid::workers::scheduler{1ms, trusting({"worker-one", "worker-two"})};
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

// liquid #416: the scheduler leases only to the workers it was configured to trust.
TEST(WorkerAuthentication, a_worker_that_is_not_trusted_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "untrusted"));
    auto leased = scheduler.lease(request("stranger", liquid::action_size_class::long_action));
    ASSERT_FALSE(leased);
    EXPECT_EQ(leased.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(scheduler.status().queued, 1U);
    EXPECT_EQ(scheduler.status().workers, 0U);
}

TEST(WorkerAuthentication, a_worker_naming_a_trusted_identity_with_another_key_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "impersonated"));
    auto forged = unsigned_request("trusted-worker", liquid::action_size_class::long_action);
    forged = fresh(identity("stranger"), liquid::workers::lease_request_purpose, forged);
    auto leased = scheduler.lease(forged);
    ASSERT_FALSE(leased);
    EXPECT_EQ(leased.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(scheduler.status().queued, 1U);
}

TEST(WorkerAuthentication, an_unsigned_worker_request_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "unsigned"));
    auto bare = unsigned_request("trusted-worker", liquid::action_size_class::long_action);
    bare.nonce = *liquid::workers::random_token();
    bare.issued_at_ms = liquid::workers::unix_milliseconds();
    auto leased = scheduler.lease(bare);
    ASSERT_FALSE(leased);
    EXPECT_EQ(leased.error().code, liquid::errc::unauthorized);
}

TEST(WorkerAuthentication, a_replayed_or_stale_worker_request_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    const auto once = request("trusted-worker", liquid::action_size_class::long_action);
    ASSERT_TRUE(scheduler.lease(once));
    auto replayed = scheduler.lease(once);
    ASSERT_FALSE(replayed);
    EXPECT_EQ(replayed.error().code, liquid::errc::unauthorized);

    auto stale = unsigned_request("trusted-worker", liquid::action_size_class::long_action);
    stale.nonce = *liquid::workers::random_token();
    stale.issued_at_ms = liquid::workers::unix_milliseconds(std::chrono::system_clock::now() -
                                                            liquid::workers::max_clock_skew - 1min);
    stale = signed_by(identity("trusted-worker"), liquid::workers::lease_request_purpose, stale);
    auto refused = scheduler.lease(stale);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}

TEST(WorkerAuthentication, lease_tokens_are_random_and_a_guessed_token_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "first"));
    ASSERT_TRUE(submit_one(scheduler, "second"));
    auto first = scheduler.lease(request("trusted-worker", liquid::action_size_class::long_action));
    auto second =
        scheduler.lease(request("trusted-worker", liquid::action_size_class::long_action));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(first->assigned);
    ASSERT_TRUE(second->assigned);
    EXPECT_EQ(first->token.size(), 32U);
    EXPECT_EQ(first->token.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_NE(first->token, second->token);
    EXPECT_EQ(first->worker, worker_id("trusted-worker"));

    for (const auto& guess :
         {std::string{"1"}, std::string{"2"}, *liquid::workers::random_token()}) {
        auto heartbeat = scheduler.heartbeat(
            fresh(identity("trusted-worker"),
                  liquid::workers::heartbeat_purpose,
                  liquid::workers::heartbeat_request{.operation = first->operation,
                                                     .worker = worker_id("trusted-worker"),
                                                     .token = guess}));
        ASSERT_TRUE(heartbeat) << heartbeat.error().message;
        EXPECT_FALSE(heartbeat->accepted);
        auto failed = scheduler.fail(
            fresh(identity("trusted-worker"),
                  liquid::workers::complete_purpose,
                  liquid::workers::complete_request{.operation = first->operation,
                                                    .worker = worker_id("trusted-worker"),
                                                    .token = guess,
                                                    .failure = "guessed"}));
        ASSERT_TRUE(failed) << failed.error().message;
        EXPECT_FALSE(failed->accepted);
    }
    auto genuine = scheduler.heartbeat(
        fresh(identity("trusted-worker"),
              liquid::workers::heartbeat_purpose,
              liquid::workers::heartbeat_request{.operation = first->operation,
                                                 .worker = worker_id("trusted-worker"),
                                                 .token = first->token}));
    ASSERT_TRUE(genuine) << genuine.error().message;
    EXPECT_TRUE(genuine->accepted);
}

TEST(WorkerAuthentication, a_heartbeat_or_completion_from_an_untrusted_worker_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "held"));
    auto lease = scheduler.lease(request("trusted-worker", liquid::action_size_class::long_action));
    ASSERT_TRUE(lease && lease->assigned);
    // Even with the real token, a request signed by another key is refused.
    auto heartbeat = scheduler.heartbeat(
        fresh(identity("stranger"),
              liquid::workers::heartbeat_purpose,
              liquid::workers::heartbeat_request{.operation = lease->operation,
                                                 .worker = worker_id("trusted-worker"),
                                                 .token = lease->token}));
    ASSERT_FALSE(heartbeat);
    EXPECT_EQ(heartbeat.error().code, liquid::errc::unauthorized);
    auto failed = scheduler.fail(
        fresh(identity("stranger"),
              liquid::workers::complete_purpose,
              liquid::workers::complete_request{.operation = lease->operation,
                                                .worker = worker_id("trusted-worker"),
                                                .token = lease->token,
                                                .failure = "forged"}));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(scheduler.status().in_flight, 1U);
}

// Builds the wire result a worker would return for `lease`, signed as `producer` by `key`.
auto worker_result(const liquid::workers::lease_response& lease,
                   const std::string& worker,
                   liquid::signer& key,
                   liquid::actor producer) -> liquid::reapi::ActionResult {
    const auto decoded = decoded_action(lease.action_digest.hash);
    auto action_bytes =
        liquid::objects::encode(liquid::objects::action_record{decoded.input, decoded.sandbox});
    if (!action_bytes) {
        throw std::runtime_error{action_bytes.error().message};
    }
    auto action = liquid::objects::digest_of(liquid::algorithm::blake3, *action_bytes);
    if (!action) {
        throw std::runtime_error{action.error().message};
    }
    const auto out = content_digest("stdout");
    const auto err = content_digest("stderr");
    auto value = liquid::action_result{.action = *action,
                                       .stdout_output = out,
                                       .stderr_output = err,
                                       .worker = worker_id(worker),
                                       .producer = std::move(producer),
                                       .record_version = 4,
                                       .schedule = lease.schedule};
    auto body = liquid::objects::encode_result_body(value);
    if (!body) {
        throw std::runtime_error{body.error().message};
    }
    auto signed_value = liquid::runtime::block_on(key.sign("result", std::move(*body)));
    if (!signed_value) {
        throw std::runtime_error{signed_value.error().message};
    }
    value.sig = *liquid::identity_detail::canonical_signature(*signed_value);
    const auto blobs = std::map<std::string, std::vector<std::byte>>{
        {liquid::to_string(out), text_bytes("stdout")},
        {liquid::to_string(err), text_bytes("stderr")}};
    auto wire = liquid::reapi::to_reapi_result(
        value,
        liquid::algorithm::blake3,
        liquid::reapi::BlobResolver{
            .bytes = [&](liquid::digest id) -> liquid::result<std::vector<std::byte>> {
                return blobs.at(liquid::to_string(id));
            }});
    if (!wire) {
        throw std::runtime_error{wire.error().message};
    }
    return *wire;
}

// liquid #416: a result is bound to the worker that holds the lease, not to the key it names.
TEST(WorkerAuthentication, a_result_signed_by_another_key_is_refused) {
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    ASSERT_TRUE(submit_one(scheduler, "bound"));
    auto lease = scheduler.lease(request("trusted-worker", liquid::action_size_class::long_action));
    ASSERT_TRUE(lease && lease->assigned);
    // The forgery names the trusted worker's identifier with the stranger's key and signature.
    auto forged_producer = identity("stranger").identity();
    forged_producer.taap_id = worker_id("trusted-worker");
    auto forged = worker_result(*lease, "trusted-worker", identity("stranger"), forged_producer);
    const auto completion = [&] {
        return fresh(identity("trusted-worker"),
                     liquid::workers::complete_purpose,
                     liquid::workers::complete_request{.operation = lease->operation,
                                                       .worker = worker_id("trusted-worker"),
                                                       .token = lease->token});
    };
    auto refused = scheduler.complete(completion(), forged);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(scheduler.status().completed, 0U);

    auto genuine = worker_result(*lease,
                                 "trusted-worker",
                                 identity("trusted-worker"),
                                 identity("trusted-worker").identity());
    auto accepted = scheduler.complete(completion(), genuine);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(accepted->accepted) << accepted->message;
    EXPECT_EQ(scheduler.status().completed, 1U);
}

// liquid #416: the worker executes only leases the pinned scheduler signed for its request.
TEST(WorkerAuthentication, the_worker_refuses_a_lease_the_scheduler_did_not_sign) {
    const auto worker = worker_id("trusted-worker");
    const auto nonce = *liquid::workers::random_token();
    const auto lease = liquid::workers::lease_response{
        .assigned = true,
        .operation = "operations/liquid-1",
        .token = *liquid::workers::random_token(),
        .action_digest = action_digest("action"),
        .schedule = schedule(liquid::action_size_class::medium, "signed"),
        .native_action = content_digest("action"),
        .worker = worker,
        .nonce = nonce};
    auto& scheduler = identity("scheduler");
    const auto scheduler_key = scheduler.identity();

    auto unsigned_lease = liquid::workers::verify_lease(lease, scheduler_key, worker, nonce);
    ASSERT_FALSE(unsigned_lease);
    EXPECT_EQ(unsigned_lease.error().code, liquid::errc::unauthorized);

    auto impostor = signed_by(identity("stranger"), liquid::workers::lease_purpose, lease);
    EXPECT_FALSE(liquid::workers::verify_lease(impostor, scheduler_key, worker, nonce));

    const auto genuine = signed_by(scheduler, liquid::workers::lease_purpose, lease);
    EXPECT_TRUE(liquid::workers::verify_lease(genuine, scheduler_key, worker, nonce));
    // A genuine lease answers one request of one worker.
    EXPECT_FALSE(liquid::workers::verify_lease(
        genuine, scheduler_key, worker, *liquid::workers::random_token()));
    EXPECT_FALSE(
        liquid::workers::verify_lease(genuine, scheduler_key, worker_id("stranger"), nonce));
    // Changing any signed field breaks the signature.
    auto tampered = genuine;
    tampered.command = text_bytes("rm -rf /");
    EXPECT_FALSE(liquid::workers::verify_lease(tampered, scheduler_key, worker, nonce));
}

TEST(WorkerAuthentication, signed_messages_survive_the_wire) {
    auto original = request("trusted-worker", liquid::action_size_class::medium);
    auto encoded = liquid::workers::encode(original);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::workers::decode<liquid::workers::worker_request>(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->nonce, original.nonce);
    EXPECT_EQ(decoded->issued_at_ms, original.issued_at_ms);
    EXPECT_EQ(decoded->signature, original.signature);
    auto scheduler = liquid::workers::scheduler{1s, trusting({"trusted-worker"})};
    EXPECT_TRUE(scheduler.lease(*decoded));
}

} // namespace
