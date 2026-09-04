#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "objects/records.hpp"

namespace {

auto from_hex(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    for (std::size_t index = 0; index + 1 < input.size(); index += 2) {
        const auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
        output.push_back(std::byte{
            static_cast<unsigned char>((digit(input[index]) << 4) | digit(input[index + 1]))});
    }
    return output;
}

auto example_digest(unsigned char value) -> liquid::digest {
    auto digest = liquid::digest{liquid::algorithm::blake3, {}};
    digest.bytes.front() = std::byte{value};
    return digest;
}

TEST(Records, empty_tree_fixture_is_canonical) {
    auto input =
        std::ifstream{std::string{LIQUID_SOURCE_DIR} + "/tests/fixtures/records/empty-tree.hex"};
    ASSERT_TRUE(input);
    auto hex = std::string{};
    input >> hex;
    auto fixture = from_hex(hex);
    auto decoded = liquid::objects::decode_tree(fixture);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(decoded->entries.empty());
    auto encoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, fixture);
    auto digest = liquid::objects::digest_of(liquid::algorithm::blake3, *encoded);
    ASSERT_TRUE(digest);
    EXPECT_EQ(digest->alg, liquid::algorithm::blake3);
}

TEST(Records, blob_records_carry_range_addressable_chunk_sizes) {
    auto chunk = liquid::digest{liquid::algorithm::blake3, {}};
    chunk.bytes.front() = std::byte{0x42};
    const auto original = liquid::blob{{chunk}, 42, {42}};
    auto encoded = liquid::objects::encode(original);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_blob(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded->chunk_sizes.size(), 1U);
    EXPECT_EQ(decoded->chunk_sizes.front(), 42U);

    const auto invalid = liquid::blob{{chunk}, 42, {41}};
    auto refused = liquid::objects::encode(invalid);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
}

TEST(Records, remote_blob_records_carry_content_and_location) {
    const auto chunk = example_digest(0x42);
    const auto original =
        liquid::blob{.chunks = {chunk},
                     .size = 42,
                     .chunk_sizes = {42},
                     .content = example_digest(0x24),
                     .remote = liquid::remote_blob_location{"worker", "instance"}};
    auto encoded = liquid::objects::encode(original);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_blob(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(liquid::to_string(decoded->content), liquid::to_string(original.content));
    EXPECT_EQ(decoded->remote, original.remote);
    auto reencoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

TEST(Records, action_records_are_canonical_and_round_trip) {
    const auto record = liquid::objects::action_record{
        .input = {.command = {"clang", "-c", "hello.c", "-o", "out/hello.o"},
                  .cwd = "src",
                  .env = {{"CCACHE_DISABLE", "1"}, {"CFLAGS", "-O2"}},
                  .outputs = {"out/hello.o"},
                  .inputs = example_digest(0x01),
                  .toolchain = example_digest(0x02),
                  .platform = {"linux", "x86_64"},
                  .timeout = std::chrono::seconds{30}},
        .sandbox = example_digest(0x03)};
    auto encoded = liquid::objects::encode(record);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_action(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->input.command, record.input.command);
    EXPECT_EQ(decoded->input.cwd, record.input.cwd);
    EXPECT_EQ(decoded->input.env, record.input.env);
    EXPECT_EQ(decoded->input.outputs, record.input.outputs);
    EXPECT_EQ(liquid::to_string(decoded->input.inputs), liquid::to_string(record.input.inputs));
    EXPECT_EQ(liquid::to_string(decoded->input.toolchain),
              liquid::to_string(record.input.toolchain));
    EXPECT_EQ(decoded->input.platform.os, record.input.platform.os);
    EXPECT_EQ(decoded->input.platform.arch, record.input.platform.arch);
    EXPECT_EQ(decoded->input.timeout, record.input.timeout);
    EXPECT_FALSE(decoded->input.remote_eligible);
    EXPECT_EQ(liquid::to_string(decoded->sandbox), liquid::to_string(record.sandbox));
    auto reencoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

TEST(Records, remote_eligible_action_round_trips_as_version_two) {
    const auto record = liquid::objects::action_record{.input = {.command = {"clang"},
                                                                 .inputs = example_digest(0x01),
                                                                 .toolchain = example_digest(0x02),
                                                                 .platform = {"linux", "x86_64"},
                                                                 .remote_eligible = true},
                                                       .sandbox = example_digest(0x03)};
    auto encoded = liquid::objects::encode(record);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_action(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(decoded->input.remote_eligible);
    auto reencoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

TEST(Records, result_records_round_trip_positive_observed_time) {
    const auto record = liquid::action_result{
        .action = example_digest(0x01),
        .exit = 0,
        .outputs = {{"out/hello.o", 0755, example_digest(0x02)}},
        .stdout_output = example_digest(0x03),
        .stderr_output = example_digest(0x04),
        .manifest = {{"hello.c", example_digest(0x05)}},
        .timing = {.queued = std::chrono::milliseconds{1},
                   .sandbox = std::chrono::milliseconds{2},
                   .exec = std::chrono::milliseconds{3},
                   .hash = std::chrono::milliseconds{4},
                   .store = std::chrono::milliseconds{5}},
        .worker = "linux/x86_64",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}}};
    auto complete = record;
    complete.started_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'001}};
    complete.finished_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'002}};
    complete.provenance = example_digest(0x06);
    auto encoded = liquid::objects::encode(complete);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_result(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(liquid::to_string(decoded->action), liquid::to_string(complete.action));
    ASSERT_EQ(decoded->outputs.size(), 1U);
    EXPECT_EQ(decoded->outputs.front().path, "out/hello.o");
    EXPECT_EQ(decoded->timing.exec, std::chrono::milliseconds{3});
    EXPECT_EQ(decoded->timing.hash, std::chrono::milliseconds{4});
    EXPECT_EQ(decoded->timing.store, std::chrono::milliseconds{5});
    EXPECT_EQ(decoded->observed_at, complete.observed_at);
    EXPECT_EQ(decoded->started_at, complete.started_at);
    EXPECT_EQ(decoded->finished_at, complete.finished_at);
    ASSERT_TRUE(decoded->provenance);
    EXPECT_EQ(liquid::to_string(*decoded->provenance), liquid::to_string(*complete.provenance));
}

TEST(Records, build_trace_round_trips_actions_edges_timing_and_cache_outcome) {
    const auto trace =
        liquid::build_trace{.wall_time = std::chrono::milliseconds{42},
                            .actions = {{.result = {example_digest(0x01)},
                                         .action = example_digest(0x02),
                                         .outputs = {"middle.o"},
                                         .timing = {.queued = std::chrono::milliseconds{1},
                                                    .sandbox = std::chrono::milliseconds{2},
                                                    .exec = std::chrono::milliseconds{20},
                                                    .hash = std::chrono::milliseconds{3},
                                                    .store = std::chrono::milliseconds{4}},
                                         .cache = liquid::cache_status::miss,
                                         .start_offset = std::chrono::milliseconds{5}},
                                        {.result = {example_digest(0x03)},
                                         .action = example_digest(0x04),
                                         .outputs = {"final.o"},
                                         .timing = {.exec = std::chrono::milliseconds{10}},
                                         .cache = liquid::cache_status::hit,
                                         .start_offset = std::chrono::milliseconds{25}}},
                            .edges = {{example_digest(0x02), example_digest(0x04)}},
                            .cache = {.hits = 1,
                                      .misses = 1,
                                      .bytes_served = 7,
                                      .bytes_rebuilt = 9,
                                      .disk = {.available = true, .bytes_avoided = 11}}};
    auto encoded = liquid::objects::encode(trace);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_build_trace(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->wall_time, trace.wall_time);
    ASSERT_EQ(decoded->actions.size(), 2U);
    EXPECT_EQ(decoded->actions.front().timing.exec, std::chrono::milliseconds{20});
    EXPECT_EQ(decoded->actions.back().cache, liquid::cache_status::hit);
    ASSERT_EQ(decoded->edges.size(), 1U);
    EXPECT_EQ(liquid::to_string(decoded->edges.front().dependency),
              liquid::to_string(trace.edges.front().dependency));
    EXPECT_EQ(decoded->cache.disk.bytes_avoided, 11U);
}

TEST(Records, legacy_result_reencodes_its_original_signing_body) {
    auto legacy = liquid::action_result{.action = example_digest(0x01),
                                        .stdout_output = example_digest(0x02),
                                        .stderr_output = example_digest(0x03),
                                        .timing = {.queued = std::chrono::milliseconds{1},
                                                   .sandbox = std::chrono::milliseconds{2},
                                                   .exec = std::chrono::milliseconds{3},
                                                   .store = std::chrono::milliseconds{4}},
                                        .worker = "legacy",
                                        .record_version = 1};
    auto encoded = liquid::objects::encode_result_body(legacy);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto signed_bytes = liquid::objects::encode(legacy);
    ASSERT_TRUE(signed_bytes) << signed_bytes.error().message;
    auto decoded = liquid::objects::decode_result(*signed_bytes);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->record_version, 1U);
    EXPECT_EQ(decoded->timing.hash, std::chrono::milliseconds{});
    EXPECT_EQ(decoded->timing.store, std::chrono::milliseconds{4});
    auto reencoded = liquid::objects::encode_result_body(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

} // namespace
