#include "liquid/reapi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include "http2/connection.hpp"
#include "http2/grpc.hpp"
#include "liquid/store.hpp"
#include "reapi/conversion.hpp"
#include "reapi/messages.hpp"
#include "repository/store_access.hpp"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr auto capabilities_method =
    "/build.bazel.remote.execution.v2.Capabilities/GetCapabilities";
constexpr auto find_missing_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/FindMissingBlobs";
constexpr auto batch_read_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchReadBlobs";
constexpr auto batch_update_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchUpdateBlobs";
constexpr auto get_tree_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/GetTree";
constexpr auto get_action_result_method =
    "/build.bazel.remote.execution.v2.ActionCache/GetActionResult";
constexpr auto update_action_result_method =
    "/build.bazel.remote.execution.v2.ActionCache/UpdateActionResult";
constexpr auto read_method = "/google.bytestream.ByteStream/Read";
constexpr auto write_method = "/google.bytestream.ByteStream/Write";
constexpr auto query_write_method = "/google.bytestream.ByteStream/QueryWriteStatus";

struct resource_digest {
    reapi::Digest wire;
    digest native;
};

struct server_state {
    server_state(store objects, std::filesystem::path action_cache, std::string instance)
        : objects(std::move(objects)), action_cache(std::move(action_cache)),
          instance(std::move(instance)) {}

    store objects;
    std::filesystem::path action_cache;
    std::string instance;
    std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> partial_writes;
};

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto header(std::span<const http2::header_field> values, std::string_view name)
    -> std::optional<std::string> {
    const auto found = std::ranges::find(values, name, &http2::header_field::name);
    return found == values.end() ? std::nullopt : std::optional<std::string>{found->value};
}

auto request_message(std::span<const std::byte> body) -> result<std::vector<std::byte>> {
    auto messages = http2::decode_grpc_messages(body);
    if (!messages) {
        return std::unexpected(messages.error());
    }
    if (messages->size() != 1 || messages->front().compressed) {
        return std::unexpected(
            server_error(errc::corrupt, "REAPI request must contain one uncompressed message"));
    }
    return std::move(messages->front().bytes);
}

auto grpc_code(errc code) -> std::uint32_t {
    switch (code) {
    case errc::not_found:
        return 5;
    case errc::invalid_argument:
    case errc::corrupt:
        return 3;
    case errc::unauthorized:
        return 7;
    case errc::unsupported:
        return 12;
    default:
        return 13;
    }
}

runtime::task<void> send_status(http2::connection& connection,
                                std::uint32_t stream,
                                std::uint32_t status,
                                std::string message,
                                std::optional<std::vector<std::byte>> response = {}) {
    const auto headers = std::array{http2::header_field{":status", "200", false},
                                    http2::header_field{"content-type", "application/grpc", false}};
    auto sent_headers = co_await connection.send_headers(stream, headers, false);
    if (!sent_headers) {
        co_return std::unexpected(sent_headers.error());
    }
    if (response) {
        auto envelope = http2::encode_grpc_message({false, std::move(*response)});
        if (!envelope) {
            co_return std::unexpected(envelope.error());
        }
        auto sent = co_await connection.send_data(stream, std::move(*envelope), false);
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
    } else {
        auto envelope = http2::encode_grpc_message({false, {}});
        if (!envelope) {
            co_return std::unexpected(envelope.error());
        }
        auto sent = co_await connection.send_data(stream, std::move(*envelope), false);
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
    }
    auto trailers =
        std::vector<http2::header_field>{{"grpc-status", std::to_string(status), false}};
    if (!message.empty()) {
        trailers.push_back({"grpc-message", std::move(message), false});
    }
    co_return co_await connection.send_headers(stream, trailers, true);
}

template <typename Message>
runtime::task<void>
send_message(http2::connection& connection, std::uint32_t stream, const Message& response) {
    auto encoded = reapi::encode(response);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await send_status(connection, stream, 0, {}, std::move(*encoded));
}

runtime::task<void>
send_failure(http2::connection& connection, std::uint32_t stream, const error& value) {
    co_return co_await send_status(
        connection, stream, grpc_code(value.code), value.message, std::nullopt);
}

auto request_algorithm(reapi::DigestFunctionValue function) -> result<algorithm> {
    if (function == reapi::DigestFunctionValue::unknown ||
        function == reapi::DigestFunctionValue::sha256) {
        return algorithm::sha256;
    }
    return reapi::digest_algorithm(function);
}

auto native_digest(const reapi::Digest& value, algorithm hash_algorithm) -> result<digest> {
    return reapi::to_liquid_digest(value, hash_algorithm);
}

auto resource(std::string_view name) -> result<resource_digest> {
    const auto marker = name.rfind("/blobs/");
    const auto start = marker == std::string_view::npos
                           ? (name.starts_with("blobs/") ? std::size_t{6} : std::string_view::npos)
                           : marker + 7;
    if (start == std::string_view::npos) {
        return std::unexpected(
            server_error(errc::invalid_argument, "invalid ByteStream resource", std::string{name}));
    }
    const auto separator = name.find('/', start);
    if (separator == std::string_view::npos) {
        return std::unexpected(
            server_error(errc::invalid_argument, "invalid ByteStream resource", std::string{name}));
    }
    const auto hash = name.substr(start, separator - start);
    const auto size_text = name.substr(separator + 1);
    auto size = std::int64_t{};
    const auto parsed =
        std::from_chars(size_text.data(), size_text.data() + size_text.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != size_text.data() + size_text.size() || size < 0) {
        return std::unexpected(
            server_error(errc::invalid_argument, "invalid ByteStream size", std::string{name}));
    }
    auto wire = reapi::Digest{.hash = std::string{hash}, .size_bytes = size};
    auto native = native_digest(wire, algorithm::sha256);
    if (!native) {
        return std::unexpected(native.error());
    }
    return resource_digest{std::move(wire), *native};
}

auto validate_instance(std::string_view configured, std::string_view requested) -> result<void> {
    if (configured != requested) {
        return std::unexpected(
            server_error(errc::not_found, "REAPI instance is unavailable", std::string{requested}));
    }
    return {};
}

auto cache_path(const server_state& state, const reapi::Digest& action) -> std::filesystem::path {
    return state.action_cache / action.hash;
}

runtime::task<void> handle_capabilities(http2::connection& connection,
                                        std::uint32_t stream,
                                        server_state& state,
                                        std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::GetCapabilitiesRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    if (!valid) {
        co_return co_await send_failure(connection, stream, valid.error());
    }
    auto response = reapi::ServerCapabilities{
        .cache_capabilities =
            reapi::CacheCapabilities{
                .digest_functions = {reapi::DigestFunctionValue::sha256},
                .action_cache_update_capabilities = reapi::ActionCacheUpdateCapabilities{true},
                .max_batch_total_size_bytes = 4U * 1024U * 1024U,
                .symlink_absolute_path_strategy = reapi::SymlinkAbsolutePathStrategy::disallowed,
                .max_cas_blob_size_bytes = std::numeric_limits<std::int64_t>::max()},
        .execution_capabilities =
            reapi::ExecutionCapabilities{.digest_function = reapi::DigestFunctionValue::sha256,
                                         .exec_enabled = false,
                                         .digest_functions = {reapi::DigestFunctionValue::sha256}},
        .low_api_version = reapi::SemVer{.major = 2},
        .high_api_version = reapi::SemVer{.major = 2, .minor = 3}};
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> handle_find_missing(http2::connection& connection,
                                        std::uint32_t stream,
                                        server_state& state,
                                        std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::FindMissingBlobsRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    auto hash_algorithm = request_algorithm(request->digest_function);
    if (!valid || !hash_algorithm) {
        co_return co_await send_failure(
            connection, stream, !valid ? valid.error() : hash_algorithm.error());
    }
    auto response = reapi::FindMissingBlobsResponse{};
    for (const auto& requested : request->blob_digests) {
        auto id = native_digest(requested, *hash_algorithm);
        if (!id) {
            co_return co_await send_failure(connection, stream, id.error());
        }
        auto present = co_await state.objects.contains(*id);
        if (!present) {
            co_return co_await send_failure(connection, stream, present.error());
        }
        if (!*present) {
            response.missing_blob_digests.push_back(requested);
        }
    }
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> handle_batch_read(http2::connection& connection,
                                      std::uint32_t stream,
                                      server_state& state,
                                      std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::BatchReadBlobsRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    auto hash_algorithm = request_algorithm(request->digest_function);
    if (!valid || !hash_algorithm) {
        co_return co_await send_failure(
            connection, stream, !valid ? valid.error() : hash_algorithm.error());
    }
    auto response = reapi::BatchReadBlobsResponse{};
    for (const auto& requested : request->digests) {
        auto entry = reapi::BatchReadBlobsResponseEntry{.digest = requested};
        auto id = native_digest(requested, *hash_algorithm);
        auto contents = id ? co_await state.objects.get(*id)
                           : result<std::vector<std::byte>>{std::unexpected(id.error())};
        if (!contents) {
            entry.status = reapi::Status{.code = contents.error().code == errc::not_found ? 5 : 13,
                                         .message = contents.error().message};
        } else if (contents->size() != static_cast<std::size_t>(requested.size_bytes)) {
            entry.status = reapi::Status{.code = 13, .message = "stored blob size is corrupt"};
        } else {
            entry.data = std::move(*contents);
            entry.status = reapi::Status{};
        }
        response.responses.push_back(std::move(entry));
    }
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> handle_batch_update(http2::connection& connection,
                                        std::uint32_t stream,
                                        server_state& state,
                                        std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::BatchUpdateBlobsRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    auto hash_algorithm = request_algorithm(request->digest_function);
    if (!valid || !hash_algorithm) {
        co_return co_await send_failure(
            connection, stream, !valid ? valid.error() : hash_algorithm.error());
    }
    auto response = reapi::BatchUpdateBlobsResponse{};
    for (auto& requested : request->requests) {
        auto entry = reapi::BatchUpdateBlobsResponseEntry{.digest = requested.digest};
        if (!requested.digest || requested.compressor != reapi::Compressor::identity) {
            entry.status = reapi::Status{.code = 3, .message = "invalid batch update entry"};
            response.responses.push_back(std::move(entry));
            continue;
        }
        auto expected = reapi::make_digest(*hash_algorithm, requested.data);
        if (!expected || expected->hash != requested.digest->hash ||
            expected->size_bytes != requested.digest->size_bytes) {
            entry.status = reapi::Status{.code = 3, .message = "blob digest does not match data"};
            response.responses.push_back(std::move(entry));
            continue;
        }
        auto stored = co_await repository_detail::store_access::put_plain(
            state.objects, std::move(requested.data), *hash_algorithm);
        entry.status =
            stored ? reapi::Status{} : reapi::Status{.code = 13, .message = stored.error().message};
        response.responses.push_back(std::move(entry));
    }
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> collect_directories(server_state& state,
                                        const reapi::Digest& requested,
                                        std::set<std::string>& seen,
                                        std::vector<reapi::Directory>& output) {
    const auto key = requested.hash + "/" + std::to_string(requested.size_bytes);
    if (!seen.insert(key).second) {
        co_return result<void>{};
    }
    auto id = native_digest(requested, algorithm::sha256);
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto bytes = co_await state.objects.get(*id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto directory = reapi::decode_message<reapi::Directory>(*bytes);
    if (!directory) {
        co_return std::unexpected(directory.error());
    }
    auto children = directory->directories;
    output.push_back(std::move(*directory));
    for (const auto& child : children) {
        if (!child.digest) {
            co_return std::unexpected(
                server_error(errc::corrupt, "directory child has no digest", child.name));
        }
        auto collected = co_await collect_directories(state, *child.digest, seen, output);
        if (!collected) {
            co_return std::unexpected(collected.error());
        }
    }
    co_return result<void>{};
}

runtime::task<void> handle_get_tree(http2::connection& connection,
                                    std::uint32_t stream,
                                    server_state& state,
                                    std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::GetTreeRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    auto hash_algorithm = request_algorithm(request->digest_function);
    if (!valid || !hash_algorithm || *hash_algorithm != algorithm::sha256 ||
        !request->root_digest) {
        co_return co_await send_failure(
            connection,
            stream,
            !valid            ? valid.error()
            : !hash_algorithm ? hash_algorithm.error()
            : !request->root_digest
                ? server_error(errc::invalid_argument, "root directory digest is missing")
                : server_error(errc::unsupported, "GetTree requires SHA-256"));
    }
    auto response = reapi::GetTreeResponse{};
    auto seen = std::set<std::string>{};
    auto collected =
        co_await collect_directories(state, *request->root_digest, seen, response.directories);
    if (!collected) {
        co_return co_await send_failure(connection, stream, collected.error());
    }
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> handle_read(http2::connection& connection,
                                std::uint32_t stream,
                                server_state& state,
                                std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::ReadRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto requested = resource(request->resource_name);
    if (!requested || request->read_offset < 0 || request->read_limit < 0) {
        co_return co_await send_failure(
            connection,
            stream,
            requested ? server_error(errc::invalid_argument, "invalid ByteStream read range")
                      : requested.error());
    }
    auto contents = co_await state.objects.get(requested->native);
    if (!contents) {
        co_return co_await send_failure(connection, stream, contents.error());
    }
    const auto offset = std::min<std::size_t>(contents->size(), request->read_offset);
    auto count = contents->size() - offset;
    if (request->read_limit != 0) {
        count = std::min<std::size_t>(count, request->read_limit);
    }
    auto response =
        reapi::ReadResponse{.data = std::vector<std::byte>{
                                contents->begin() + static_cast<std::ptrdiff_t>(offset),
                                contents->begin() + static_cast<std::ptrdiff_t>(offset + count)}};
    co_return co_await send_message(connection, stream, response);
}

runtime::task<void> handle_write(http2::connection& connection,
                                 std::uint32_t stream,
                                 server_state& state,
                                 std::span<const std::byte> body) {
    auto messages = http2::decode_grpc_messages(body);
    if (!messages || messages->empty()) {
        co_return co_await send_failure(
            connection,
            stream,
            messages ? server_error(errc::corrupt, "ByteStream Write has no messages")
                     : messages.error());
    }
    auto committed = std::int64_t{};
    auto resource_name = std::string{};
    for (const auto& message : *messages) {
        if (message.compressed) {
            co_return co_await send_failure(
                connection,
                stream,
                server_error(errc::unsupported, "compressed ByteStream Write is unsupported"));
        }
        auto request = reapi::decode_message<reapi::WriteRequest>(message.bytes);
        if (!request) {
            co_return co_await send_failure(connection, stream, request.error());
        }
        if (request->resource_name.empty()) {
            request->resource_name = resource_name;
        } else if (!resource_name.empty() && request->resource_name != resource_name) {
            co_return co_await send_failure(
                connection,
                stream,
                server_error(errc::invalid_argument, "write resource changed within a stream"));
        } else {
            resource_name = request->resource_name;
        }
        auto requested = resource(request->resource_name);
        if (!requested || request->write_offset < 0) {
            co_return co_await send_failure(
                connection,
                stream,
                requested ? server_error(errc::invalid_argument, "invalid ByteStream write offset")
                          : requested.error());
        }
        auto complete = std::vector<std::byte>{};
        auto offset_matches = true;
        {
            auto lock = std::scoped_lock{state.mutex};
            auto& pending = state.partial_writes[request->resource_name];
            if (static_cast<std::size_t>(request->write_offset) != pending.size()) {
                offset_matches = false;
            } else {
                pending.insert(pending.end(), request->data.begin(), request->data.end());
                if (request->finish_write) {
                    complete = std::move(pending);
                    state.partial_writes.erase(request->resource_name);
                }
            }
        }
        if (!offset_matches) {
            co_return co_await send_failure(
                connection,
                stream,
                server_error(errc::invalid_argument, "write offset does not match"));
        }
        committed = request->write_offset + static_cast<std::int64_t>(request->data.size());
        if (request->finish_write) {
            auto actual = reapi::make_digest(algorithm::sha256, complete);
            if (!actual || actual->hash != requested->wire.hash ||
                actual->size_bytes != requested->wire.size_bytes) {
                co_return co_await send_failure(
                    connection,
                    stream,
                    server_error(errc::invalid_argument, "written blob digest does not match"));
            }
            auto stored = co_await repository_detail::store_access::put_plain(
                state.objects, std::move(complete), algorithm::sha256);
            if (!stored) {
                co_return co_await send_failure(connection, stream, stored.error());
            }
        }
    }
    co_return co_await send_message(
        connection, stream, reapi::WriteResponse{.committed_size = committed});
}

runtime::task<void> handle_query_write(http2::connection& connection,
                                       std::uint32_t stream,
                                       server_state& state,
                                       std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::QueryWriteStatusRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto requested = resource(request->resource_name);
    if (!requested) {
        co_return co_await send_failure(connection, stream, requested.error());
    }
    auto present = co_await state.objects.contains(requested->native);
    if (!present) {
        co_return co_await send_failure(connection, stream, present.error());
    }
    if (*present) {
        co_return co_await send_message(
            connection,
            stream,
            reapi::QueryWriteStatusResponse{.committed_size = requested->wire.size_bytes,
                                            .complete = true});
    }
    auto committed = std::int64_t{};
    auto found_write = false;
    {
        auto lock = std::scoped_lock{state.mutex};
        const auto found = state.partial_writes.find(request->resource_name);
        if (found != state.partial_writes.end()) {
            committed = static_cast<std::int64_t>(found->second.size());
            found_write = true;
        }
    }
    if (!found_write) {
        co_return co_await send_failure(
            connection, stream, server_error(errc::not_found, "write resource is unknown"));
    }
    co_return co_await send_message(
        connection,
        stream,
        reapi::QueryWriteStatusResponse{.committed_size = committed, .complete = false});
}

runtime::task<void> handle_get_action(http2::connection& connection,
                                      std::uint32_t stream,
                                      server_state& state,
                                      std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::GetActionResultRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    if (!valid || !request->action_digest) {
        co_return co_await send_failure(
            connection,
            stream,
            !valid ? valid.error()
                   : server_error(errc::invalid_argument, "action digest is missing"));
    }
    auto cached = co_await store_detail::read_file(cache_path(state, *request->action_digest));
    if (!cached) {
        auto value = cached.error();
        if (value.code == errc::io) {
            value.code = errc::not_found;
            value.message = "action result is not cached";
        }
        co_return co_await send_failure(connection, stream, value);
    }
    auto response = reapi::decode_message<reapi::ActionResult>(*cached);
    if (!response) {
        co_return co_await send_failure(connection, stream, response.error());
    }
    co_return co_await send_message(connection, stream, *response);
}

runtime::task<void> handle_update_action(http2::connection& connection,
                                         std::uint32_t stream,
                                         server_state& state,
                                         std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::UpdateActionResultRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    if (!valid || !request->action_digest || !request->action_result) {
        co_return co_await send_failure(
            connection,
            stream,
            !valid ? valid.error()
                   : server_error(errc::invalid_argument, "action result request is incomplete"));
    }
    auto encoded = reapi::encode(*request->action_result);
    if (!encoded) {
        co_return co_await send_failure(connection, stream, encoded.error());
    }
    auto written = co_await store_detail::write_file(cache_path(state, *request->action_digest),
                                                     std::move(*encoded));
    if (!written) {
        co_return co_await send_failure(connection, stream, written.error());
    }
    co_return co_await send_message(connection, stream, *request->action_result);
}

runtime::task<void> dispatch(http2::connection& connection,
                             std::uint32_t stream,
                             server_state& state,
                             std::string_view method,
                             std::span<const std::byte> bytes) {
    if (method == capabilities_method) {
        co_return co_await handle_capabilities(connection, stream, state, bytes);
    }
    if (method == find_missing_method) {
        co_return co_await handle_find_missing(connection, stream, state, bytes);
    }
    if (method == batch_read_method) {
        co_return co_await handle_batch_read(connection, stream, state, bytes);
    }
    if (method == batch_update_method) {
        co_return co_await handle_batch_update(connection, stream, state, bytes);
    }
    if (method == get_tree_method) {
        co_return co_await handle_get_tree(connection, stream, state, bytes);
    }
    if (method == read_method) {
        co_return co_await handle_read(connection, stream, state, bytes);
    }
    if (method == query_write_method) {
        co_return co_await handle_query_write(connection, stream, state, bytes);
    }
    if (method == get_action_result_method) {
        co_return co_await handle_get_action(connection, stream, state, bytes);
    }
    if (method == update_action_result_method) {
        co_return co_await handle_update_action(connection, stream, state, bytes);
    }
    co_return co_await send_status(connection, stream, 12, "method is not implemented");
}

runtime::task<void> session(std::shared_ptr<server_state> state,
                            std::shared_ptr<boost::corosio::tcp_socket> socket) {
    auto stream = boost::capy::any_stream{socket.get()};
    auto connection = http2::connection{stream, http2::endpoint_role::server};
    auto started = co_await connection.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    for (;;) {
        auto stream_id = co_await connection.receive_stream();
        if (!stream_id) {
            co_return std::unexpected(stream_id.error());
        }
        auto headers = co_await connection.receive_headers(*stream_id);
        if (!headers) {
            co_return std::unexpected(headers.error());
        }
        auto method = header(*headers, ":path");
        auto content_type = header(*headers, "content-type");
        if (!method || !content_type || !content_type->starts_with("application/grpc")) {
            auto failed = co_await send_status(connection, *stream_id, 3, "request is not gRPC");
            if (!failed) {
                co_return std::unexpected(failed.error());
            }
            continue;
        }
        auto body = co_await connection.receive_data(*stream_id);
        if (!body) {
            co_return std::unexpected(body.error());
        }
        if (*method == write_method) {
            auto handled = co_await handle_write(connection, *stream_id, *state, *body);
            if (!handled) {
                co_return std::unexpected(handled.error());
            }
            continue;
        }
        auto message = request_message(*body);
        if (!message) {
            auto failed = co_await send_failure(connection, *stream_id, message.error());
            if (!failed) {
                co_return std::unexpected(failed.error());
            }
            continue;
        }
        auto handled = co_await dispatch(connection, *stream_id, *state, *method, *message);
        if (!handled) {
            co_return std::unexpected(handled.error());
        }
    }
}

} // namespace

runtime::task<void> serve_reapi(reapi_serve_options options) {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(options.listen, endpoint)) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, code.message(), options.listen));
    }
    if (endpoint.port() == 0) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "REAPI listen port must be non-zero"));
    }
    auto opened = co_await store::open(options.store_directory);
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto code = std::error_code{};
    auto action_cache = options.store_directory / "reapi-action-cache";
    std::filesystem::create_directories(action_cache, code);
    if (code) {
        co_return std::unexpected(server_error(errc::io, code.message(), action_cache.string()));
    }
    auto state = std::make_shared<server_state>(
        std::move(*opened), std::move(action_cache), std::move(options.instance_name));
    const auto executor = co_await boost::capy::this_coro::executor;
    auto acceptor = boost::corosio::tcp_acceptor{executor.context(), endpoint};
    for (;;) {
        auto socket = std::make_shared<boost::corosio::tcp_socket>(executor.context());
        const auto [accept_error] = co_await acceptor.accept(*socket);
        if (accept_error) {
            co_return std::unexpected(
                server_error(errc::io, accept_error.message(), options.listen));
        }
        runtime::spawn(session(state, std::move(socket)), [](result<void> outcome) {
            if (!outcome && outcome.error().message != "End of file" &&
                outcome.error().message != "peer has sent GOAWAY") {
                std::println(stderr,
                             "REAPI connection error: {} ({})",
                             outcome.error().message,
                             outcome.error().context);
            }
        });
    }
}

} // namespace liquid
