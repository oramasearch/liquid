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
    EXPECT_EQ(liquid::to_string(decoded->sandbox), liquid::to_string(record.sandbox));
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
                   .upload = std::chrono::milliseconds{4}},
        .worker = "linux/x86_64",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}}};
    auto encoded = liquid::objects::encode(record);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_result(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(liquid::to_string(decoded->action), liquid::to_string(record.action));
    ASSERT_EQ(decoded->outputs.size(), 1U);
    EXPECT_EQ(decoded->outputs.front().path, "out/hello.o");
    EXPECT_EQ(decoded->timing.exec, std::chrono::milliseconds{3});
    EXPECT_EQ(decoded->observed_at, record.observed_at);
}

} // namespace
