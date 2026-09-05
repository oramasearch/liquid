#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "reapi/messages.hpp"

namespace {

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

template <typename Message> void expect_round_trip(std::string_view name) {
    SCOPED_TRACE(name);
    const auto bytes = fixture(name);
    auto decoded = liquid::reapi::decode_message<Message>(bytes);
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto encoded = liquid::reapi::encode(*decoded);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(*encoded, bytes);
}

TEST(ReapiMessages, official_protobuf_fixtures_round_trip_byte_exactly) {
    using namespace liquid::reapi;
    expect_round_trip<Any>("any.bin");
    expect_round_trip<Timestamp>("timestamp.bin");
    expect_round_trip<Duration>("duration.bin");
    expect_round_trip<Status>("status.bin");
    expect_round_trip<Operation>("operation.bin");
    expect_round_trip<Digest>("digest.bin");
    expect_round_trip<DigestFunction>("digest-function.bin");
    expect_round_trip<NodeProperties>("node-properties.bin");
    expect_round_trip<FileNode>("file-node.bin");
    expect_round_trip<DirectoryNode>("directory-node.bin");
    expect_round_trip<SymlinkNode>("symlink-node.bin");
    expect_round_trip<Directory>("directory.bin");
    expect_round_trip<Platform>("platform.bin");
    expect_round_trip<Command>("command.bin");
    expect_round_trip<Action>("action.bin");
    expect_round_trip<ExecutedActionMetadata>("executed-action-metadata.bin");
    expect_round_trip<OutputFile>("output-file.bin");
    expect_round_trip<OutputDirectory>("output-directory.bin");
    expect_round_trip<OutputSymlink>("output-symlink.bin");
    expect_round_trip<Tree>("tree.bin");
    expect_round_trip<ActionResult>("action-result.bin");
    expect_round_trip<ExecuteRequest>("execute-request.bin");
    expect_round_trip<ExecuteResponse>("execute-response.bin");
    expect_round_trip<ExecuteOperationMetadata>("execute-operation-metadata.bin");
    expect_round_trip<WaitExecutionRequest>("wait-execution-request.bin");
    expect_round_trip<GetActionResultRequest>("get-action-result-request.bin");
    expect_round_trip<UpdateActionResultRequest>("update-action-result-request.bin");
    expect_round_trip<FindMissingBlobsRequest>("find-missing-blobs-request.bin");
    expect_round_trip<FindMissingBlobsResponse>("find-missing-blobs-response.bin");
    expect_round_trip<BatchUpdateBlobsRequest>("batch-update-blobs-request.bin");
    expect_round_trip<BatchUpdateBlobsResponse>("batch-update-blobs-response.bin");
    expect_round_trip<BatchReadBlobsRequest>("batch-read-blobs-request.bin");
    expect_round_trip<BatchReadBlobsResponse>("batch-read-blobs-response.bin");
    expect_round_trip<GetTreeRequest>("get-tree-request.bin");
    expect_round_trip<GetTreeResponse>("get-tree-response.bin");
    expect_round_trip<GetCapabilitiesRequest>("get-capabilities-request.bin");
    expect_round_trip<CacheCapabilities>("cache-capabilities.bin");
    expect_round_trip<ExecutionCapabilities>("execution-capabilities.bin");
    expect_round_trip<ServerCapabilities>("server-capabilities.bin");
    expect_round_trip<ReadRequest>("read-request.bin");
    expect_round_trip<ReadResponse>("read-response.bin");
    expect_round_trip<WriteRequest>("write-request.bin");
    expect_round_trip<WriteResponse>("write-response.bin");
    expect_round_trip<QueryWriteStatusRequest>("query-write-status-request.bin");
    expect_round_trip<QueryWriteStatusResponse>("query-write-status-response.bin");
}

TEST(ReapiMessages, unknown_fields_survive_decode_and_encode) {
    const auto bytes = fixture("digest.bin");
    auto decoded = liquid::reapi::decode_message<liquid::reapi::Digest>(bytes);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_FALSE(decoded->unknown_fields.empty());
    auto encoded = liquid::reapi::encode(*decoded);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(*encoded, bytes);
}

} // namespace
