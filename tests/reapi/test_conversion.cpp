#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "objects/records.hpp"
#include "reapi/conversion.hpp"

namespace {

using namespace std::chrono_literals;

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto input = std::span{value};
    const auto raw = std::as_bytes(input);
    return {raw.begin(), raw.end()};
}

auto fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_REAPI_FIXTURES} / name;
    auto input = std::ifstream{path, std::ios::binary};
    EXPECT_TRUE(input) << path;
    const auto chars =
        std::vector<char>{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto output = std::vector<std::byte>{};
    output.reserve(chars.size());
    for (const auto value : chars) {
        output.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    return output;
}

auto digest(std::string_view seed, liquid::algorithm hash_algorithm = liquid::algorithm::blake3)
    -> liquid::digest {
    auto value = liquid::hash(hash_algorithm, bytes(seed));
    EXPECT_TRUE(value) << value.error().message;
    return *value;
}

auto entry(std::string name, std::uint32_t mode, liquid::node_kind kind, liquid::digest ref)
    -> liquid::tree_entry {
    return {.name = std::move(name), .mode = mode, .kind = kind, .ref = ref};
}

TEST(ReapiConversion, directory_matches_official_protobuf_reference_and_caches_subtrees) {
    const auto hello = digest("hello-record");
    const auto script = digest("script-record");
    const auto nested = digest("nested-record");
    const auto link = digest("link-record");
    const auto child = digest("child-tree-record");
    const auto root = digest("root-tree-record");
    const auto second_root = digest("second-root-tree-record");
    const auto root_tree =
        liquid::tree{.entries = {
                         entry("sub", 0040755, liquid::node_kind::tree, child),
                         entry("run.sh", 0100755, liquid::node_kind::blob, script),
                         entry("hello-link", 0120000, liquid::node_kind::blob, link),
                         entry("hello.txt", 0100644, liquid::node_kind::blob, hello),
                     }};
    auto trees = std::map<std::string, liquid::tree>{
        {liquid::to_string(root), root_tree},
        {liquid::to_string(second_root), root_tree},
        {liquid::to_string(child),
         liquid::tree{.entries =
                          {
                              entry("nested.txt", 0100644, liquid::node_kind::blob, nested),
                          }}},
    };
    auto blobs = std::map<std::string, std::vector<std::byte>>{
        {liquid::to_string(hello), bytes("hello\n")},
        {liquid::to_string(script), bytes("#!/bin/sh\necho hi\n")},
        {liquid::to_string(nested), bytes("nested\n")},
        {liquid::to_string(link), bytes("hello.txt")},
    };
    auto tree_reads = std::size_t{};
    auto blob_reads = std::size_t{};
    const auto source = liquid::reapi::TreeSource{
        .tree = [&](liquid::digest value) -> liquid::result<liquid::tree> {
            ++tree_reads;
            return trees.at(liquid::to_string(value));
        },
        .blob = [&](liquid::digest value) -> liquid::result<std::vector<std::byte>> {
            ++blob_reads;
            return blobs.at(liquid::to_string(value));
        },
    };

    auto cache = liquid::reapi::DirectoryCache{};
    auto converted = cache.convert(root, liquid::algorithm::sha256, source);
    ASSERT_TRUE(converted) << converted.error().message;
    ASSERT_EQ(converted->objects.size(), 2U);
    EXPECT_EQ(converted->objects.front().bytes, fixture("directory-reference-child.bin"));
    EXPECT_EQ(converted->objects.back().bytes, fixture("directory-reference.bin"));
    EXPECT_EQ(converted->root_digest.hash,
              "060b0d269f3e9e96094cec473650cce96bafe6fc5a2771a3723922bb587bab06");
    EXPECT_EQ(tree_reads, 2U);
    EXPECT_EQ(blob_reads, 4U);
    EXPECT_EQ(cache.size(), 2U);

    auto repeated = cache.convert(root, liquid::algorithm::sha256, source);
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(tree_reads, 2U);
    EXPECT_EQ(blob_reads, 4U);

    auto shared_child = cache.convert(second_root, liquid::algorithm::sha256, source);
    ASSERT_TRUE(shared_child) << shared_child.error().message;
    EXPECT_EQ(tree_reads, 3U);
    EXPECT_EQ(blob_reads, 7U);
    EXPECT_EQ(cache.size(), 3U);
}

TEST(ReapiConversion, digest_selection_prefers_blake3_and_falls_back_to_sha256) {
    using liquid::reapi::DigestFunctionValue;
    auto capabilities = liquid::reapi::ServerCapabilities{
        .cache_capabilities =
            liquid::reapi::CacheCapabilities{
                .digest_functions = {DigestFunctionValue::sha256, DigestFunctionValue::blake3}},
        .execution_capabilities =
            liquid::reapi::ExecutionCapabilities{.digest_functions = {DigestFunctionValue::sha256}},
    };
    auto cache = liquid::reapi::select_digest_algorithm(capabilities, false);
    auto execution = liquid::reapi::select_digest_algorithm(capabilities, true);
    ASSERT_TRUE(cache) << cache.error().message;
    ASSERT_TRUE(execution) << execution.error().message;
    EXPECT_EQ(*cache, liquid::algorithm::blake3);
    EXPECT_EQ(*execution, liquid::algorithm::sha256);
}

TEST(ReapiConversion, actions_round_trip_through_command_and_action_messages) {
    const auto input = liquid::action_input{
        .command = {"clang++", "-c", "hello.cpp"},
        .cwd = "src",
        .env = {{"LANG", "C.UTF-8"}},
        .outputs = {"hello.o"},
        .inputs = digest("inputs"),
        .toolchain = digest("toolchain"),
        .platform = {.os = "linux", .arch = "aarch64"},
        .timeout = 30s,
        .remote_eligible = true,
    };
    const auto sandbox = digest("sandbox");
    auto root = liquid::reapi::make_digest(liquid::algorithm::blake3, bytes("directory"));
    ASSERT_TRUE(root) << root.error().message;
    auto encoded = liquid::reapi::to_reapi_action(input, *root, sandbox, liquid::algorithm::blake3);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::reapi::from_reapi_action(
        encoded->action, encoded->command, liquid::reapi::DigestFunctionValue::blake3);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->input.command, input.command);
    EXPECT_EQ(decoded->input.cwd, input.cwd);
    EXPECT_EQ(decoded->input.env, input.env);
    EXPECT_EQ(decoded->input.outputs, input.outputs);
    EXPECT_EQ(liquid::to_string(decoded->input.inputs), liquid::to_string(input.inputs));
    EXPECT_EQ(liquid::to_string(decoded->input.toolchain), liquid::to_string(input.toolchain));
    EXPECT_EQ(decoded->input.platform.os, input.platform.os);
    EXPECT_EQ(decoded->input.platform.arch, input.platform.arch);
    EXPECT_EQ(decoded->input.timeout, input.timeout);
    EXPECT_TRUE(decoded->input.remote_eligible);
    EXPECT_EQ(liquid::to_string(decoded->sandbox), liquid::to_string(sandbox));
}

TEST(ReapiConversion, results_fill_standard_fields_and_preserve_the_native_record) {
    const auto file = digest("file-record");
    const auto link = digest("link-record");
    const auto directory = digest("directory-record");
    const auto stdout_output = digest("stdout-record");
    const auto stderr_output = digest("stderr-record");
    const auto value = liquid::action_result{
        .action = digest("action"),
        .exit = 7,
        .outputs = {{.path = "file", .mode = 0755, .blob = file},
                    {.path = "link", .mode = 0120000, .blob = link},
                    {.path = "tree", .mode = 0040755, .blob = directory}},
        .stdout_output = stdout_output,
        .stderr_output = stderr_output,
        .manifest = {{.path = "input", .content = digest("observed")}},
        .timing = {.queued = 5ms, .sandbox = 6ms, .exec = 1507ms, .hash = 8ms, .store = 9ms},
        .worker = "worker-01",
        .observed_at = std::chrono::sys_seconds{1700000010s},
        .started_at = std::chrono::sys_seconds{1700000000s},
        .finished_at = std::chrono::sys_seconds{1700000002s},
        .provenance = digest("provenance"),
        .record_version = 3,
        .sboms = {digest("sbom")},
    };
    const auto content = std::map<std::string, std::vector<std::byte>>{
        {liquid::to_string(file), bytes("executable")},
        {liquid::to_string(link), bytes("file")},
        {liquid::to_string(directory), bytes("encoded-directory")},
        {liquid::to_string(stdout_output), bytes("stdout\n")},
        {liquid::to_string(stderr_output), bytes("stderr\n")},
    };
    const auto resolver = liquid::reapi::BlobResolver{
        .bytes = [&](liquid::digest key) -> liquid::result<std::vector<std::byte>> {
            return content.at(liquid::to_string(key));
        },
    };

    auto encoded = liquid::reapi::to_reapi_result(value, liquid::algorithm::blake3, resolver);
    ASSERT_TRUE(encoded) << encoded.error().message;
    ASSERT_EQ(encoded->output_files.size(), 1U);
    ASSERT_TRUE(encoded->output_files.front().node_properties);
    ASSERT_TRUE(encoded->output_files.front().node_properties->unix_mode);
    EXPECT_EQ(encoded->output_files.front().node_properties->unix_mode->value, 0100755U);
    ASSERT_EQ(encoded->output_directories.size(), 1U);
    ASSERT_EQ(encoded->output_symlinks.size(), 1U);
    EXPECT_TRUE(encoded->stdout_digest);
    EXPECT_TRUE(encoded->stderr_digest);
    ASSERT_TRUE(encoded->execution_metadata);
    ASSERT_EQ(encoded->execution_metadata->auxiliary_metadata.size(), 1U);

    auto wire = liquid::reapi::encode(*encoded);
    ASSERT_TRUE(wire) << wire.error().message;
    auto from_wire = liquid::reapi::decode_message<liquid::reapi::ActionResult>(*wire);
    ASSERT_TRUE(from_wire) << from_wire.error().message;

    auto decoded =
        liquid::reapi::from_reapi_result(*from_wire, value.action, liquid::algorithm::blake3);
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto original_record = liquid::objects::encode(value);
    auto decoded_record = liquid::objects::encode(*decoded);
    ASSERT_TRUE(original_record) << original_record.error().message;
    ASSERT_TRUE(decoded_record) << decoded_record.error().message;
    EXPECT_EQ(*decoded_record, *original_record);
}

TEST(ReapiConversion, standard_results_decode_without_liquid_metadata) {
    auto value = liquid::reapi::ActionResult{
        .output_files = {{.path = "empty",
                          .contents = {},
                          .node_properties =
                              liquid::reapi::NodeProperties{
                                  .unix_mode = liquid::reapi::UInt32Value{.value = 0100644}}}},
        .exit_code = 2,
        .stdout_raw = bytes("stdout"),
        .stderr_raw = bytes("stderr"),
        .execution_metadata =
            liquid::reapi::ExecutedActionMetadata{
                .worker = "stock-worker",
                .execution_start_timestamp = liquid::reapi::Timestamp{.seconds = 100},
                .execution_completed_timestamp = liquid::reapi::Timestamp{.seconds = 101}},
    };
    const auto action = digest("foreign-action");
    auto decoded = liquid::reapi::from_reapi_result(value, action, liquid::algorithm::sha256);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->exit, 2);
    EXPECT_EQ(decoded->worker, "stock-worker");
    ASSERT_EQ(decoded->outputs.size(), 1U);
    EXPECT_EQ(decoded->outputs.front().path, "empty");
    EXPECT_EQ(decoded->outputs.front().mode, 0644U);
    EXPECT_EQ(decoded->started_at, std::chrono::sys_seconds{100s});
    EXPECT_EQ(decoded->finished_at, std::chrono::sys_seconds{101s});
}

} // namespace
