// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::reapi {

using UnknownFields = std::vector<std::byte>;

struct Any {
    std::string type_url;
    std::vector<std::byte> value;
    UnknownFields unknown_fields;
};

struct Timestamp {
    std::int64_t seconds{};
    std::int32_t nanos{};
    UnknownFields unknown_fields;
};

struct Duration {
    std::int64_t seconds{};
    std::int32_t nanos{};
    UnknownFields unknown_fields;
};

struct UInt32Value {
    std::uint32_t value{};
    UnknownFields unknown_fields;
};

struct Status {
    std::int32_t code{};
    std::string message;
    std::vector<Any> details;
    UnknownFields unknown_fields;
};

struct Operation {
    std::string name;
    std::optional<Any> metadata;
    bool done{};
    std::variant<std::monostate, Status, Any> result;
    UnknownFields unknown_fields;
};

struct Empty {
    UnknownFields unknown_fields;
};

struct CancelOperationRequest {
    std::string name;
    UnknownFields unknown_fields;
};

struct SemVer {
    std::int32_t major{};
    std::int32_t minor{};
    std::int32_t patch{};
    std::string prerelease;
    UnknownFields unknown_fields;
};

enum class DigestFunctionValue : std::int32_t {
    unknown = 0,
    sha256 = 1,
    sha1 = 2,
    md5 = 3,
    vso = 4,
    sha384 = 5,
    sha512 = 6,
    murmur3 = 7,
    sha256tree = 8,
    blake3 = 9,
    git_sha1 = 10,
};

struct DigestFunction {
    UnknownFields unknown_fields;
};

enum class Compressor : std::int32_t {
    identity = 0,
    zstd = 1,
    deflate = 2,
    brotli = 3,
};

enum class SymlinkAbsolutePathStrategy : std::int32_t {
    unknown = 0,
    disallowed = 1,
    allowed = 2,
};

enum class ExecutionStage : std::int32_t {
    unknown = 0,
    cache_check = 1,
    queued = 2,
    executing = 3,
    completed = 4,
};

enum class OutputDirectoryFormat : std::int32_t {
    tree_only = 0,
    directory_only = 1,
    tree_and_directory = 2,
};

struct Digest {
    std::string hash;
    std::int64_t size_bytes{};
    UnknownFields unknown_fields;
};

struct NodeProperty {
    std::string name;
    std::string value;
    UnknownFields unknown_fields;
};

struct NodeProperties {
    std::vector<NodeProperty> properties;
    std::optional<Timestamp> mtime;
    std::optional<UInt32Value> unix_mode;
    UnknownFields unknown_fields;
};

struct FileNode {
    std::string name;
    std::optional<Digest> digest;
    bool is_executable{};
    std::optional<NodeProperties> node_properties;
    UnknownFields unknown_fields;
};

struct DirectoryNode {
    std::string name;
    std::optional<Digest> digest;
    UnknownFields unknown_fields;
};

struct SymlinkNode {
    std::string name;
    std::string target;
    std::optional<NodeProperties> node_properties;
    UnknownFields unknown_fields;
};

struct Directory {
    std::vector<FileNode> files;
    std::vector<DirectoryNode> directories;
    std::vector<SymlinkNode> symlinks;
    std::optional<NodeProperties> node_properties;
    UnknownFields unknown_fields;
};

struct EnvironmentVariable {
    std::string name;
    std::string value;
    UnknownFields unknown_fields;
};

struct PlatformProperty {
    std::string name;
    std::string value;
    UnknownFields unknown_fields;
};

struct Platform {
    std::vector<PlatformProperty> properties;
    UnknownFields unknown_fields;
};

struct Command {
    std::vector<std::string> arguments;
    std::vector<EnvironmentVariable> environment_variables;
    std::vector<std::string> output_files;
    std::vector<std::string> output_directories;
    std::optional<Platform> platform;
    std::string working_directory;
    std::vector<std::string> output_paths;
    std::vector<std::string> output_node_properties;
    OutputDirectoryFormat output_directory_format{OutputDirectoryFormat::tree_only};
    UnknownFields unknown_fields;
};

struct Action {
    std::optional<Digest> command_digest;
    std::optional<Digest> input_root_digest;
    std::optional<Duration> timeout;
    bool do_not_cache{};
    std::vector<std::byte> salt;
    std::optional<Platform> platform;
    UnknownFields unknown_fields;
};

struct ExecutedActionMetadata {
    std::string worker;
    std::optional<Timestamp> queued_timestamp;
    std::optional<Timestamp> worker_start_timestamp;
    std::optional<Timestamp> worker_completed_timestamp;
    std::optional<Timestamp> input_fetch_start_timestamp;
    std::optional<Timestamp> input_fetch_completed_timestamp;
    std::optional<Timestamp> execution_start_timestamp;
    std::optional<Timestamp> execution_completed_timestamp;
    std::optional<Timestamp> output_upload_start_timestamp;
    std::optional<Timestamp> output_upload_completed_timestamp;
    std::vector<Any> auxiliary_metadata;
    std::optional<Duration> virtual_execution_duration;
    UnknownFields unknown_fields;
};

struct OutputFile {
    std::string path;
    std::optional<Digest> digest;
    bool is_executable{};
    std::vector<std::byte> contents;
    std::optional<NodeProperties> node_properties;
    UnknownFields unknown_fields;
};

struct OutputDirectory {
    std::string path;
    std::optional<Digest> tree_digest;
    bool is_topologically_sorted{};
    std::optional<Digest> root_directory_digest;
    UnknownFields unknown_fields;
};

struct OutputSymlink {
    std::string path;
    std::string target;
    std::optional<NodeProperties> node_properties;
    UnknownFields unknown_fields;
};

struct Tree {
    std::optional<Directory> root;
    std::vector<Directory> children;
    UnknownFields unknown_fields;
};

struct LogFile {
    std::optional<Digest> digest;
    bool human_readable{};
    UnknownFields unknown_fields;
};

struct ActionResult {
    std::vector<OutputFile> output_files;
    std::vector<OutputDirectory> output_directories;
    std::int32_t exit_code{};
    std::vector<std::byte> stdout_raw;
    std::optional<Digest> stdout_digest;
    std::vector<std::byte> stderr_raw;
    std::optional<Digest> stderr_digest;
    std::optional<ExecutedActionMetadata> execution_metadata;
    std::vector<OutputSymlink> output_file_symlinks;
    std::vector<OutputSymlink> output_directory_symlinks;
    std::vector<OutputSymlink> output_symlinks;
    UnknownFields unknown_fields;
};

struct ExecutionPolicy {
    std::int32_t priority{};
    UnknownFields unknown_fields;
};

struct ResultsCachePolicy {
    std::int32_t priority{};
    UnknownFields unknown_fields;
};

struct ExecuteRequest {
    std::string instance_name;
    bool skip_cache_lookup{};
    std::optional<Digest> action_digest;
    std::optional<ExecutionPolicy> execution_policy;
    std::optional<ResultsCachePolicy> results_cache_policy;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    bool inline_stdout{};
    bool inline_stderr{};
    std::vector<std::string> inline_output_files;
    UnknownFields unknown_fields;
};

struct ExecuteResponse {
    std::optional<ActionResult> result;
    bool cached_result{};
    std::optional<Status> status;
    std::map<std::string, LogFile> server_logs;
    std::string message;
    UnknownFields unknown_fields;
};

struct ExecuteOperationMetadata {
    ExecutionStage stage{ExecutionStage::unknown};
    std::optional<Digest> action_digest;
    std::string stdout_stream_name;
    std::string stderr_stream_name;
    std::optional<ExecutedActionMetadata> partial_execution_metadata;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct WaitExecutionRequest {
    std::string name;
    UnknownFields unknown_fields;
};

struct GetActionResultRequest {
    std::string instance_name;
    std::optional<Digest> action_digest;
    bool inline_stdout{};
    bool inline_stderr{};
    std::vector<std::string> inline_output_files;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct UpdateActionResultRequest {
    std::string instance_name;
    std::optional<Digest> action_digest;
    std::optional<ActionResult> action_result;
    std::optional<ResultsCachePolicy> results_cache_policy;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct FindMissingBlobsRequest {
    std::string instance_name;
    std::vector<Digest> blob_digests;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct FindMissingBlobsResponse {
    std::vector<Digest> missing_blob_digests;
    UnknownFields unknown_fields;
};

struct BatchUpdateBlobsRequestEntry {
    std::optional<Digest> digest;
    std::vector<std::byte> data;
    Compressor compressor{Compressor::identity};
    UnknownFields unknown_fields;
};

struct BatchUpdateBlobsRequest {
    std::string instance_name;
    std::vector<BatchUpdateBlobsRequestEntry> requests;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct BatchUpdateBlobsResponseEntry {
    std::optional<Digest> digest;
    std::optional<Status> status;
    UnknownFields unknown_fields;
};

struct BatchUpdateBlobsResponse {
    std::vector<BatchUpdateBlobsResponseEntry> responses;
    UnknownFields unknown_fields;
};

struct BatchReadBlobsRequest {
    std::string instance_name;
    std::vector<Digest> digests;
    std::vector<Compressor> acceptable_compressors;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct BatchReadBlobsResponseEntry {
    std::optional<Digest> digest;
    std::vector<std::byte> data;
    std::optional<Status> status;
    Compressor compressor{Compressor::identity};
    UnknownFields unknown_fields;
};

struct BatchReadBlobsResponse {
    std::vector<BatchReadBlobsResponseEntry> responses;
    UnknownFields unknown_fields;
};

struct GetTreeRequest {
    std::string instance_name;
    std::optional<Digest> root_digest;
    std::int32_t page_size{};
    std::string page_token;
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    UnknownFields unknown_fields;
};

struct GetTreeResponse {
    std::vector<Directory> directories;
    std::string next_page_token;
    UnknownFields unknown_fields;
};

struct GetCapabilitiesRequest {
    std::string instance_name;
    UnknownFields unknown_fields;
};

struct ActionCacheUpdateCapabilities {
    bool update_enabled{};
    UnknownFields unknown_fields;
};

struct PriorityRange {
    std::int32_t min_priority{};
    std::int32_t max_priority{};
    UnknownFields unknown_fields;
};

struct PriorityCapabilities {
    std::vector<PriorityRange> priorities;
    UnknownFields unknown_fields;
};

struct FastCdc2020Params {
    std::uint64_t avg_chunk_size_bytes{};
    std::uint32_t seed{};
    UnknownFields unknown_fields;
};

struct RepMaxCdcParams {
    std::uint64_t min_chunk_size_bytes{};
    std::uint64_t horizon_size_bytes{};
    UnknownFields unknown_fields;
};

struct CacheCapabilities {
    std::vector<DigestFunctionValue> digest_functions;
    std::optional<ActionCacheUpdateCapabilities> action_cache_update_capabilities;
    std::optional<PriorityCapabilities> cache_priority_capabilities;
    std::int64_t max_batch_total_size_bytes{};
    SymlinkAbsolutePathStrategy symlink_absolute_path_strategy{
        SymlinkAbsolutePathStrategy::unknown};
    std::vector<Compressor> supported_compressors;
    std::vector<Compressor> supported_batch_update_compressors;
    std::int64_t max_cas_blob_size_bytes{};
    bool split_blob_support{};
    bool splice_blob_support{};
    std::optional<FastCdc2020Params> fast_cdc_2020_params;
    std::optional<RepMaxCdcParams> rep_max_cdc_params;
    UnknownFields unknown_fields;
};

struct ExecutionCapabilities {
    DigestFunctionValue digest_function{DigestFunctionValue::unknown};
    bool exec_enabled{};
    std::optional<PriorityCapabilities> execution_priority_capabilities;
    std::vector<std::string> supported_node_properties;
    std::vector<DigestFunctionValue> digest_functions;
    UnknownFields unknown_fields;
};

struct ServerCapabilities {
    std::optional<CacheCapabilities> cache_capabilities;
    std::optional<ExecutionCapabilities> execution_capabilities;
    std::optional<SemVer> deprecated_api_version;
    std::optional<SemVer> low_api_version;
    std::optional<SemVer> high_api_version;
    UnknownFields unknown_fields;
};

struct ReadRequest {
    std::string resource_name;
    std::int64_t read_offset{};
    std::int64_t read_limit{};
    UnknownFields unknown_fields;
};

struct ReadResponse {
    std::vector<std::byte> data;
    UnknownFields unknown_fields;
};

struct WriteRequest {
    std::string resource_name;
    std::int64_t write_offset{};
    bool finish_write{};
    std::vector<std::byte> data;
    UnknownFields unknown_fields;
};

struct WriteResponse {
    std::int64_t committed_size{};
    UnknownFields unknown_fields;
};

struct QueryWriteStatusRequest {
    std::string resource_name;
    UnknownFields unknown_fields;
};

struct QueryWriteStatusResponse {
    std::int64_t committed_size{};
    bool complete{};
    UnknownFields unknown_fields;
};

#define LIQUID_REAPI_CODEC(Type)                                                                   \
    [[nodiscard]] auto encode(const Type& value) -> result<std::vector<std::byte>>;                \
    [[nodiscard]] auto decode(std::span<const std::byte> input, Type& output) -> result<void>

LIQUID_REAPI_CODEC(Any);
LIQUID_REAPI_CODEC(Timestamp);
LIQUID_REAPI_CODEC(Duration);
LIQUID_REAPI_CODEC(UInt32Value);
LIQUID_REAPI_CODEC(Status);
LIQUID_REAPI_CODEC(Operation);
LIQUID_REAPI_CODEC(Empty);
LIQUID_REAPI_CODEC(CancelOperationRequest);
LIQUID_REAPI_CODEC(SemVer);
LIQUID_REAPI_CODEC(DigestFunction);
LIQUID_REAPI_CODEC(Digest);
LIQUID_REAPI_CODEC(NodeProperty);
LIQUID_REAPI_CODEC(NodeProperties);
LIQUID_REAPI_CODEC(FileNode);
LIQUID_REAPI_CODEC(DirectoryNode);
LIQUID_REAPI_CODEC(SymlinkNode);
LIQUID_REAPI_CODEC(Directory);
LIQUID_REAPI_CODEC(EnvironmentVariable);
LIQUID_REAPI_CODEC(PlatformProperty);
LIQUID_REAPI_CODEC(Platform);
LIQUID_REAPI_CODEC(Command);
LIQUID_REAPI_CODEC(Action);
LIQUID_REAPI_CODEC(ExecutedActionMetadata);
LIQUID_REAPI_CODEC(OutputFile);
LIQUID_REAPI_CODEC(OutputDirectory);
LIQUID_REAPI_CODEC(OutputSymlink);
LIQUID_REAPI_CODEC(Tree);
LIQUID_REAPI_CODEC(LogFile);
LIQUID_REAPI_CODEC(ActionResult);
LIQUID_REAPI_CODEC(ExecutionPolicy);
LIQUID_REAPI_CODEC(ResultsCachePolicy);
LIQUID_REAPI_CODEC(ExecuteRequest);
LIQUID_REAPI_CODEC(ExecuteResponse);
LIQUID_REAPI_CODEC(ExecuteOperationMetadata);
LIQUID_REAPI_CODEC(WaitExecutionRequest);
LIQUID_REAPI_CODEC(GetActionResultRequest);
LIQUID_REAPI_CODEC(UpdateActionResultRequest);
LIQUID_REAPI_CODEC(FindMissingBlobsRequest);
LIQUID_REAPI_CODEC(FindMissingBlobsResponse);
LIQUID_REAPI_CODEC(BatchUpdateBlobsRequestEntry);
LIQUID_REAPI_CODEC(BatchUpdateBlobsRequest);
LIQUID_REAPI_CODEC(BatchUpdateBlobsResponseEntry);
LIQUID_REAPI_CODEC(BatchUpdateBlobsResponse);
LIQUID_REAPI_CODEC(BatchReadBlobsRequest);
LIQUID_REAPI_CODEC(BatchReadBlobsResponseEntry);
LIQUID_REAPI_CODEC(BatchReadBlobsResponse);
LIQUID_REAPI_CODEC(GetTreeRequest);
LIQUID_REAPI_CODEC(GetTreeResponse);
LIQUID_REAPI_CODEC(GetCapabilitiesRequest);
LIQUID_REAPI_CODEC(ActionCacheUpdateCapabilities);
LIQUID_REAPI_CODEC(PriorityRange);
LIQUID_REAPI_CODEC(PriorityCapabilities);
LIQUID_REAPI_CODEC(FastCdc2020Params);
LIQUID_REAPI_CODEC(RepMaxCdcParams);
LIQUID_REAPI_CODEC(CacheCapabilities);
LIQUID_REAPI_CODEC(ExecutionCapabilities);
LIQUID_REAPI_CODEC(ServerCapabilities);
LIQUID_REAPI_CODEC(ReadRequest);
LIQUID_REAPI_CODEC(ReadResponse);
LIQUID_REAPI_CODEC(WriteRequest);
LIQUID_REAPI_CODEC(WriteResponse);
LIQUID_REAPI_CODEC(QueryWriteStatusRequest);
LIQUID_REAPI_CODEC(QueryWriteStatusResponse);

#undef LIQUID_REAPI_CODEC

template <typename Message>
[[nodiscard]] auto decode_message(std::span<const std::byte> input) -> result<Message> {
    auto output = Message{};
    auto decoded = decode(input, output);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return output;
}

} // namespace liquid::reapi
