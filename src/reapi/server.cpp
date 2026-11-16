// SPDX-License-Identifier: MIT

#include "liquid/reapi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>

#include "http2/connection.hpp"
#include "http2/grpc.hpp"
#include "identity/taap.hpp"
#include "liquid/listener.hpp"
#include "liquid/store.hpp"
#include "liquid/workers.hpp"
#include "objects/records.hpp"
#include "reapi/action_cache.hpp"
#include "reapi/conversion.hpp"
#include "reapi/messages.hpp"
#include "reapi/uploads.hpp"
#include "repository/store_access.hpp"
#include "sandbox/environment.hpp"
#include "store/pack.hpp"
#include "workers/auth.hpp"
#include "workers/control.hpp"
#include "workers/protocol.hpp"
#include "workers/scheduler.hpp"

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
constexpr auto execute_method = "/build.bazel.remote.execution.v2.Execution/Execute";
constexpr auto wait_method = "/build.bazel.remote.execution.v2.Execution/WaitExecution";
constexpr auto cancel_method = "/google.longrunning.Operations/CancelOperation";

struct resource_digest {
    reapi::Digest wire;
};

// liquid#451: no request stream is read whole. The batch size the server advertises; a unary
// request is one message of at most that plus room for its own fields (digests, names), so a
// full BatchUpdateBlobs fits and a larger body is refused before it is held.
constexpr auto max_batch_total_size_bytes = std::size_t{4} * 1024 * 1024;
constexpr auto max_unary_message_bytes = max_batch_total_size_bytes + std::size_t{1024} * 1024;
// A ByteStream Write is read one message at a time, and each message is applied to the partial
// write before the next is read, so one stream holds at most one message of this size.
constexpr auto max_write_message_bytes = max_unary_message_bytes;

struct server_state {
    server_state(store objects,
                 std::filesystem::path action_cache,
                 std::string instance,
                 std::shared_ptr<workers::scheduler> executions,
                 std::shared_ptr<signer> lease_signer,
                 reapi_upload_limits limits)
        : objects(std::move(objects)), action_cache(std::move(action_cache)),
          instance(std::move(instance)), executions(std::move(executions)),
          lease_signer(std::move(lease_signer)), upload_timeout(limits.idle_timeout),
          partial_writes(limits), expiry([this](std::stop_token stop) {
              auto mutex = std::mutex{};
              auto condition = std::condition_variable_any{};
              auto lock = std::unique_lock{mutex};
              while (!stop.stop_requested()) {
                  condition.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
                  partial_writes.expire();
              }
          }) {}

    store objects;
    std::filesystem::path action_cache;
    std::string instance;
    std::shared_ptr<workers::scheduler> executions;
    std::shared_ptr<signer> lease_signer;
    std::chrono::milliseconds upload_timeout;
    reapi::partial_uploads partial_writes;
    std::jthread expiry;
    // When non-empty, every RPC must carry `authorization: Bearer <token>` (liquid#415).
    std::string bearer_token;
};

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

// A request stream read one DATA frame at a time into a bounded message reader.
class request_stream {
public:
    request_stream(http2::connection& connection, std::uint32_t id, std::size_t max_message_bytes)
        : connection_(connection), id_(id), reader_(max_message_bytes) {}

    // The next message, or nothing once the peer has ended the stream.
    runtime::task<std::optional<http2::grpc_message>> next() {
        for (;;) {
            auto message = reader_.next();
            if (!message) {
                co_return std::unexpected(message.error());
            }
            if (*message) {
                co_return std::move(*message);
            }
            if (ended_) {
                if (reader_.buffered() != 0) {
                    co_return std::unexpected(
                        server_error(errc::corrupt, "gRPC message body is incomplete"));
                }
                co_return std::optional<http2::grpc_message>{};
            }
            auto part = co_await connection_.receive_data_part(id_);
            if (!part) {
                co_return std::unexpected(part.error());
            }
            ended_ = part->end_stream;
            auto appended = reader_.append(part->bytes);
            if (!appended) {
                co_return std::unexpected(appended.error());
            }
        }
    }

    // Whether the peer has ended the stream and every byte of it has been read.
    [[nodiscard]] auto finished() const noexcept -> bool {
        return ended_ && reader_.buffered() == 0;
    }

    // Whether a message was refused for its size.
    [[nodiscard]] auto exceeded() const noexcept -> bool {
        return reader_.exceeded();
    }

    [[nodiscard]] auto limit() const noexcept -> std::size_t {
        return reader_.max_message_bytes();
    }

private:
    http2::connection& connection_;
    std::uint32_t id_;
    http2::grpc_message_reader reader_;
    bool ended_{};
};

auto header(std::span<const http2::header_field> values, std::string_view name)
    -> std::optional<std::string> {
    const auto found = std::ranges::find(values, name, &http2::header_field::name);
    return found == values.end() ? std::nullopt : std::optional<std::string>{found->value};
}

// A unary request: one uncompressed message, then the end of the stream.
runtime::task<std::vector<std::byte>> request_message(request_stream& request) {
    auto first = co_await request.next();
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (!*first || (*first)->compressed) {
        co_return std::unexpected(
            server_error(errc::corrupt, "REAPI request must contain one uncompressed message"));
    }
    auto rest = co_await request.next();
    if (!rest) {
        co_return std::unexpected(rest.error());
    }
    if (*rest) {
        co_return std::unexpected(
            server_error(errc::corrupt, "REAPI request must contain one uncompressed message"));
    }
    co_return std::move((*first)->bytes);
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
    case errc::busy:
        return 8;
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

template <typename Message>
runtime::task<void>
send_worker_message(http2::connection& connection, std::uint32_t stream, const Message& response) {
    auto encoded = workers::encode(response);
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
    return resource_digest{std::move(wire)};
}

auto resource_id(const resource_digest& value, algorithm hash_algorithm) -> result<digest> {
    return native_digest(value.wire, hash_algorithm);
}

runtime::task<std::pair<digest, std::vector<std::byte>>>
read_resource(server_state& state, const resource_digest& requested) {
    const auto algorithms = state.executions ? std::array{algorithm::blake3, algorithm::sha256}
                                             : std::array{algorithm::sha256, algorithm::sha256};
    auto last = server_error(errc::not_found, "ByteStream resource is unavailable");
    for (const auto hash_algorithm : algorithms) {
        auto id = resource_id(requested, hash_algorithm);
        if (!id) {
            last = id.error();
            continue;
        }
        auto contents = co_await state.objects.get(*id);
        if (contents) {
            co_return std::pair{*id, std::move(*contents)};
        }
        last = contents.error();
    }
    co_return std::unexpected(last);
}

auto written_algorithm(const resource_digest& requested,
                       std::span<const std::byte> contents,
                       bool scheduler) -> result<algorithm> {
    const auto algorithms = scheduler ? std::array{algorithm::blake3, algorithm::sha256}
                                      : std::array{algorithm::sha256, algorithm::sha256};
    for (const auto hash_algorithm : algorithms) {
        auto actual = reapi::make_digest(hash_algorithm, contents);
        if (actual && actual->hash == requested.wire.hash &&
            actual->size_bytes == requested.wire.size_bytes) {
            return hash_algorithm;
        }
    }
    return std::unexpected(
        server_error(errc::invalid_argument, "written blob digest does not match"));
}

auto host_platform() -> platform {
#if defined(__linux__)
    constexpr auto os = "linux";
#elif defined(__APPLE__)
    constexpr auto os = "macos";
#elif defined(_WIN32)
    constexpr auto os = "windows";
#else
    constexpr auto os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr auto arch = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    constexpr auto arch = "arm64";
#else
    constexpr auto arch = "unknown";
#endif
    return {os, arch};
}

auto platform_property(const reapi::Platform& source, std::string_view name)
    -> std::optional<std::string> {
    const auto found = std::ranges::find(source.properties, name, &reapi::PlatformProperty::name);
    return found == source.properties.end() ? std::nullopt
                                            : std::optional<std::string>{found->value};
}

auto safe_node_name(std::string_view name) -> bool {
    return !name.empty() && name != "." && name != ".." && !name.contains('/') &&
           !name.contains('\\');
}

runtime::task<digest>
store_blob_record(server_state& state, std::vector<std::byte> contents, algorithm hash_algorithm) {
    const auto size = contents.size();
    auto content = co_await repository_detail::store_access::put_plain(
        state.objects, std::move(contents), hash_algorithm);
    if (!content) {
        co_return std::unexpected(content.error());
    }
    auto record =
        blob{.chunks = {*content}, .size = size, .chunk_sizes = {size}, .content = *content};
    auto encoded = objects::encode(record);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::store_access::put_plain(
        state.objects, std::move(*encoded), hash_algorithm);
}

runtime::task<digest> import_directory(server_state& state,
                                       const reapi::Digest& requested,
                                       algorithm hash_algorithm,
                                       std::map<std::string, digest>& cache,
                                       std::set<std::string>& active) {
    const auto key = requested.hash + "/" + std::to_string(requested.size_bytes);
    if (const auto found = cache.find(key); found != cache.end()) {
        co_return found->second;
    }
    if (!active.insert(key).second) {
        co_return std::unexpected(
            server_error(errc::corrupt, "REAPI input directory contains a cycle", key));
    }
    auto directory_id = native_digest(requested, hash_algorithm);
    auto directory_bytes =
        directory_id ? co_await state.objects.get(*directory_id)
                     : result<std::vector<std::byte>>{std::unexpected(directory_id.error())};
    auto directory = directory_bytes
                         ? reapi::decode_message<reapi::Directory>(*directory_bytes)
                         : result<reapi::Directory>{std::unexpected(directory_bytes.error())};
    if (!directory) {
        active.erase(key);
        co_return std::unexpected(directory.error());
    }
    auto value = tree{};
    for (const auto& file : directory->files) {
        if (!safe_node_name(file.name) || !file.digest) {
            active.erase(key);
            co_return std::unexpected(
                server_error(errc::corrupt, "REAPI input file is invalid", file.name));
        }
        auto content_id = native_digest(*file.digest, hash_algorithm);
        auto contents = content_id
                            ? co_await state.objects.get(*content_id)
                            : result<std::vector<std::byte>>{std::unexpected(content_id.error())};
        if (!contents || contents->size() != static_cast<std::size_t>(file.digest->size_bytes)) {
            active.erase(key);
            co_return std::unexpected(
                contents ? server_error(errc::corrupt, "REAPI input file size changed", file.name)
                         : contents.error());
        }
        auto record = co_await store_blob_record(state, std::move(*contents), hash_algorithm);
        if (!record) {
            active.erase(key);
            co_return std::unexpected(record.error());
        }
        value.entries.push_back({.name = file.name,
                                 .mode = file.is_executable ? 0100755U : 0100644U,
                                 .kind = node_kind::blob,
                                 .id = *record,
                                 .ref = *record});
    }
    for (const auto& child : directory->directories) {
        if (!safe_node_name(child.name) || !child.digest) {
            active.erase(key);
            co_return std::unexpected(
                server_error(errc::corrupt, "REAPI input directory is invalid", child.name));
        }
        auto nested =
            co_await import_directory(state, *child.digest, hash_algorithm, cache, active);
        if (!nested) {
            active.erase(key);
            co_return std::unexpected(nested.error());
        }
        value.entries.push_back({.name = child.name,
                                 .mode = 0040755U,
                                 .kind = node_kind::tree,
                                 .id = *nested,
                                 .ref = *nested});
    }
    for (const auto& link : directory->symlinks) {
        if (!safe_node_name(link.name) || link.target.starts_with('/')) {
            active.erase(key);
            co_return std::unexpected(
                server_error(errc::corrupt, "REAPI input symlink is invalid", link.name));
        }
        auto contents = std::vector<std::byte>{
            reinterpret_cast<const std::byte*>(link.target.data()),
            reinterpret_cast<const std::byte*>(link.target.data() + link.target.size())};
        auto record = co_await store_blob_record(state, std::move(contents), hash_algorithm);
        if (!record) {
            active.erase(key);
            co_return std::unexpected(record.error());
        }
        value.entries.push_back({.name = link.name,
                                 .mode = 0120777U,
                                 .kind = node_kind::blob,
                                 .id = *record,
                                 .ref = *record});
    }
    auto encoded = objects::encode(value);
    auto stored = encoded ? co_await repository_detail::store_access::put_plain(
                                state.objects, std::move(*encoded), hash_algorithm)
                          : result<digest>{std::unexpected(encoded.error())};
    active.erase(key);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    cache.insert_or_assign(key, *stored);
    co_return *stored;
}

runtime::task<reapi::DecodedAction> import_standard_action(server_state& state,
                                                           const reapi::Action& action,
                                                           const reapi::Command& command,
                                                           algorithm hash_algorithm) {
    if (!action.command_digest || !action.input_root_digest) {
        co_return std::unexpected(server_error(errc::corrupt, "REAPI Action is missing a digest"));
    }
    auto command_bytes = reapi::encode(command);
    auto actual_command = command_bytes
                              ? reapi::make_digest(hash_algorithm, *command_bytes)
                              : result<reapi::Digest>{std::unexpected(command_bytes.error())};
    if (!actual_command || actual_command->hash != action.command_digest->hash ||
        actual_command->size_bytes != action.command_digest->size_bytes) {
        co_return std::unexpected(
            actual_command ? server_error(errc::corrupt, "REAPI Command digest does not match")
                           : actual_command.error());
    }
    auto cache = std::map<std::string, digest>{};
    auto active = std::set<std::string>{};
    auto inputs =
        co_await import_directory(state, *action.input_root_digest, hash_algorithm, cache, active);
    if (!inputs) {
        co_return std::unexpected(inputs.error());
    }
    auto target = host_platform();
    const auto* properties = action.platform    ? &*action.platform
                             : command.platform ? &*command.platform
                                                : nullptr;
    if (properties) {
        target.os = platform_property(*properties, "OSFamily").value_or(target.os);
        target.arch = platform_property(*properties, "ISA").value_or(target.arch);
    }
    auto sandbox = sandbox_detail::definition(target);
    auto sandbox_bytes = sandbox ? objects::encode(*sandbox)
                                 : result<std::vector<std::byte>>{std::unexpected(sandbox.error())};
    auto sandbox_id = sandbox_bytes ? co_await repository_detail::store_access::put_plain(
                                          state.objects, std::move(*sandbox_bytes), hash_algorithm)
                                    : result<digest>{std::unexpected(sandbox_bytes.error())};
    if (!sandbox_id) {
        co_return std::unexpected(sandbox_id.error());
    }
    auto environment = std::map<std::string, std::string>{};
    constexpr auto fixed =
        std::array{"PATH", "HOME", "TMPDIR", "LANG", "TZ", "SOURCE_DATE_EPOCH", "ZERO_AR_DATE"};
    for (const auto& item : command.environment_variables) {
        if (!std::ranges::contains(fixed, item.name)) {
            environment.insert_or_assign(item.name, item.value);
        }
    }
    auto outputs = command.output_paths;
    if (outputs.empty()) {
        outputs = command.output_files;
        outputs.insert(
            outputs.end(), command.output_directories.begin(), command.output_directories.end());
    }
    auto timeout = std::optional<std::chrono::seconds>{};
    if (action.timeout) {
        if (action.timeout->seconds < 0 || action.timeout->nanos != 0) {
            co_return std::unexpected(server_error(
                errc::corrupt, "REAPI Action timeout is not a whole non-negative second"));
        }
        timeout = std::chrono::seconds{action.timeout->seconds};
    }
    co_return reapi::DecodedAction{.input = {.command = command.arguments,
                                             .cwd = command.working_directory,
                                             .env = std::move(environment),
                                             .outputs = std::move(outputs),
                                             .inputs = *inputs,
                                             .toolchain = *inputs,
                                             .platform = std::move(target),
                                             .timeout = timeout,
                                             .remote_eligible = true},
                                   .sandbox = *sandbox_id,
                                   .input_root_digest = *action.input_root_digest};
}

auto validate_instance(std::string_view configured, std::string_view requested) -> result<void> {
    if (configured != requested) {
        return std::unexpected(
            server_error(errc::not_found, "REAPI instance is unavailable", std::string{requested}));
    }
    return {};
}

// The ActionCache entry for an action digest of `function` (liquid #428): a malformed digest is
// refused here, before any file is named.
auto cache_entry(const reapi::Digest& action, reapi::DigestFunctionValue function)
    -> result<std::string> {
    auto hash_algorithm = request_algorithm(function);
    if (!hash_algorithm) {
        return std::unexpected(server_error(errc::invalid_argument,
                                            hash_algorithm.error().message,
                                            hash_algorithm.error().context));
    }
    return reapi::action_cache_name(action, *hash_algorithm);
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
    const auto scheduler = state.executions != nullptr;
    auto response = reapi::ServerCapabilities{
        .cache_capabilities =
            reapi::CacheCapabilities{
                .digest_functions = scheduler ? std::vector{reapi::DigestFunctionValue::blake3,
                                                            reapi::DigestFunctionValue::sha256}
                                              : std::vector{reapi::DigestFunctionValue::sha256},
                .action_cache_update_capabilities = reapi::ActionCacheUpdateCapabilities{true},
                .max_batch_total_size_bytes = static_cast<std::int64_t>(max_batch_total_size_bytes),
                .symlink_absolute_path_strategy = reapi::SymlinkAbsolutePathStrategy::disallowed,
                .max_cas_blob_size_bytes = std::numeric_limits<std::int64_t>::max()},
        .execution_capabilities =
            reapi::ExecutionCapabilities{
                .digest_function = scheduler ? reapi::DigestFunctionValue::blake3
                                             : reapi::DigestFunctionValue::sha256,
                .exec_enabled = scheduler,
                .digest_functions = scheduler ? std::vector{reapi::DigestFunctionValue::blake3,
                                                            reapi::DigestFunctionValue::sha256}
                                              : std::vector{reapi::DigestFunctionValue::sha256}},
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
                                        algorithm hash_algorithm,
                                        std::set<std::string>& seen,
                                        std::vector<reapi::Directory>& output) {
    const auto key = requested.hash + "/" + std::to_string(requested.size_bytes);
    if (!seen.insert(key).second) {
        co_return result<void>{};
    }
    auto id = native_digest(requested, hash_algorithm);
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
        auto collected =
            co_await collect_directories(state, *child.digest, hash_algorithm, seen, output);
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
    if (!valid || !hash_algorithm || !request->root_digest) {
        co_return co_await send_failure(
            connection,
            stream,
            !valid ? valid.error()
            : !hash_algorithm
                ? hash_algorithm.error()
                : server_error(errc::invalid_argument, "root directory digest is missing"));
    }
    auto response = reapi::GetTreeResponse{};
    auto seen = std::set<std::string>{};
    auto collected = co_await collect_directories(
        state, *request->root_digest, *hash_algorithm, seen, response.directories);
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
    auto loaded = co_await read_resource(state, *requested);
    if (!loaded) {
        co_return co_await send_failure(connection, stream, loaded.error());
    }
    auto& contents = loaded->second;
    const auto offset = std::min<std::size_t>(contents.size(), request->read_offset);
    auto count = contents.size() - offset;
    if (request->read_limit != 0) {
        count = std::min<std::size_t>(count, request->read_limit);
    }
    auto response =
        reapi::ReadResponse{.data = std::vector<std::byte>{
                                contents.begin() + static_cast<std::ptrdiff_t>(offset),
                                contents.begin() + static_cast<std::ptrdiff_t>(offset + count)}};
    co_return co_await send_message(connection, stream, response);
}

// Awaits one step of a ByteStream Write within the upload timeout (liquid #444). A client that
// stops sending, or withholds response flow-control credit, cannot keep its upload's budget
// charge: the step is cancelled, `expired` is set, and the caller closes the connection.
template <class T>
runtime::task<T>
within_upload_timeout(const server_state& state, runtime::task<T> step, bool& expired) {
    auto [deadline_error, outcome] =
        co_await runtime::timeout(runtime::as_io(std::move(step)), state.upload_timeout);
    if (deadline_error) {
        expired = true;
        co_return std::unexpected(
            server_error(errc::unavailable, "ByteStream Write handling timed out"));
    }
    co_return std::move(outcome);
}

// Reads a Write one bounded message at a time and applies each to its tracked upload before the
// next is read (liquid#451, liquid #444). A finishing message must be the last of its RPC, so
// its bytes are stored only once the stream has ended. The result is the committed size; an
// error is the RPC's failure, unless `expired` is set.
runtime::task<std::int64_t> receive_write(server_state& state,
                                          request_stream& messages,
                                          std::unique_ptr<reapi::partial_uploads::write>& pending,
                                          bool& expired) {
    auto committed = std::int64_t{};
    auto resource_name = std::string{};
    auto count = std::size_t{};
    auto finished = std::optional<std::pair<resource_digest, std::vector<std::byte>>>{};
    for (;;) {
        auto next = co_await within_upload_timeout(state, messages.next(), expired);
        if (expired) {
            co_return std::unexpected(next.error());
        }
        if (!next && messages.exceeded()) {
            co_return std::unexpected(server_error(
                errc::busy,
                std::format("ByteStream Write message exceeds {} bytes", messages.limit())));
        }
        if (!next) {
            co_return std::unexpected(next.error());
        }
        if (!*next) {
            break;
        }
        if (finished) {
            co_return std::unexpected(
                server_error(errc::invalid_argument, "messages follow finish_write"));
        }
        const auto message = std::move(**next);
        ++count;
        if (message.compressed) {
            co_return std::unexpected(
                server_error(errc::unsupported, "compressed ByteStream Write is unsupported"));
        }
        auto request = reapi::decode_message<reapi::WriteRequest>(message.bytes);
        if (!request) {
            co_return std::unexpected(request.error());
        }
        if (request->resource_name.empty()) {
            request->resource_name = resource_name;
        } else if (!resource_name.empty() && request->resource_name != resource_name) {
            co_return std::unexpected(
                server_error(errc::invalid_argument, "write resource changed within a stream"));
        } else {
            resource_name = request->resource_name;
        }
        auto requested = resource(request->resource_name);
        if (!requested) {
            co_return std::unexpected(requested.error());
        }
        if (!pending) {
            auto started = state.partial_writes.begin(resource_name, requested->wire.size_bytes);
            if (!started) {
                co_return std::unexpected(started.error());
            }
            pending = std::move(*started);
        }
        auto appended = pending->append(request->write_offset, request->data);
        if (!appended) {
            co_return std::unexpected(appended.error());
        }
        committed = *appended;
        if (request->finish_write) {
            auto complete = pending->finish();
            if (!complete) {
                co_return std::unexpected(complete.error());
            }
            finished.emplace(std::move(*requested), std::move(*complete));
        }
    }
    if (count == 0) {
        co_return std::unexpected(server_error(errc::corrupt, "ByteStream Write has no messages"));
    }
    if (finished) {
        auto& [requested, complete] = *finished;
        auto hash_algorithm = written_algorithm(requested, complete, state.executions != nullptr);
        if (!hash_algorithm) {
            co_return std::unexpected(hash_algorithm.error());
        }
        auto stored =
            co_await within_upload_timeout(state,
                                           repository_detail::store_access::put_plain(
                                               state.objects, std::move(complete), *hash_algorithm),
                                           expired);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    co_return committed;
}

// The upload stays owned by this Write until its response is delivered; a partial upload is then
// retained for resumption, and on any failure it is released (liquid #444).
runtime::task<void> handle_write(http2::connection& connection,
                                 std::uint32_t stream,
                                 server_state& state,
                                 request_stream& messages) {
    auto pending = std::unique_ptr<reapi::partial_uploads::write>{};
    auto expired = false;
    auto received = co_await receive_write(state, messages, pending, expired);
    if (expired) {
        co_return std::unexpected(received.error());
    }
    if (!received) {
        auto failed = co_await within_upload_timeout(
            state, send_failure(connection, stream, received.error()), expired);
        co_return failed;
    }
    auto sent = co_await within_upload_timeout(
        state,
        send_message(connection, stream, reapi::WriteResponse{.committed_size = *received}),
        expired);
    if (sent) {
        pending->retain();
    }
    co_return sent;
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
    auto loaded = co_await read_resource(state, *requested);
    if (loaded) {
        co_return co_await send_message(
            connection,
            stream,
            reapi::QueryWriteStatusResponse{.committed_size = requested->wire.size_bytes,
                                            .complete = true});
    }
    if (loaded.error().code != errc::not_found) {
        co_return co_await send_failure(connection, stream, loaded.error());
    }
    auto committed = state.partial_writes.committed(request->resource_name);
    if (!committed) {
        co_return co_await send_failure(connection, stream, committed.error());
    }
    co_return co_await send_message(
        connection,
        stream,
        reapi::QueryWriteStatusResponse{.committed_size = *committed, .complete = false});
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
    auto entry = cache_entry(*request->action_digest, request->digest_function);
    if (!entry) {
        co_return co_await send_failure(connection, stream, entry.error());
    }
    auto cached = reapi::read_action_cache(state.action_cache, *entry);
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
    auto entry = cache_entry(*request->action_digest, request->digest_function);
    if (!entry) {
        co_return co_await send_failure(connection, stream, entry.error());
    }
    auto encoded = reapi::encode(*request->action_result);
    if (!encoded) {
        co_return co_await send_failure(connection, stream, encoded.error());
    }
    auto written = reapi::write_action_cache(state.action_cache, *entry, *encoded);
    if (!written) {
        co_return co_await send_failure(connection, stream, written.error());
    }
    co_return co_await send_message(connection, stream, *request->action_result);
}

runtime::task<reapi::Operation> submit_execution(server_state& state,
                                                 const reapi::Digest& action_digest,
                                                 reapi::DigestFunctionValue function,
                                                 execution_schedule schedule,
                                                 std::optional<digest> native_action = {},
                                                 std::optional<bool> remote_eligible = {}) {
    if (!state.executions) {
        co_return std::unexpected(server_error(errc::unsupported, "REAPI execution is disabled"));
    }
    auto hash_algorithm = request_algorithm(function);
    if (!hash_algorithm) {
        co_return std::unexpected(hash_algorithm.error());
    }
    // A queued action's digest later names its ActionCache entry, so it is validated now.
    auto cache_name = reapi::action_cache_name(action_digest, *hash_algorithm);
    if (!cache_name) {
        co_return std::unexpected(cache_name.error());
    }
    auto action_id = native_digest(action_digest, *hash_algorithm);
    if (!action_id) {
        co_return std::unexpected(action_id.error());
    }
    auto action_bytes = co_await state.objects.get(*action_id);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = reapi::decode_message<reapi::Action>(*action_bytes);
    if (!action || !action->command_digest) {
        co_return std::unexpected(
            action ? server_error(errc::corrupt, "REAPI action has no command digest")
                   : action.error());
    }
    auto command_id = native_digest(*action->command_digest, *hash_algorithm);
    if (!command_id) {
        co_return std::unexpected(command_id.error());
    }
    auto command_bytes = co_await state.objects.get(*command_id);
    if (!command_bytes) {
        co_return std::unexpected(command_bytes.error());
    }
    auto command = reapi::decode_message<reapi::Command>(*command_bytes);
    if (!command) {
        co_return std::unexpected(command.error());
    }
    const auto effective_function = function == reapi::DigestFunctionValue::unknown
                                        ? reapi::DigestFunctionValue::sha256
                                        : function;
    auto decoded = reapi::from_reapi_action(*action, *command, effective_function);
    auto queued_action = std::move(*action_bytes);
    auto queued_command = std::move(*command_bytes);
    if (!decoded) {
        decoded = co_await import_standard_action(state, *action, *command, *hash_algorithm);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        auto messages = reapi::to_reapi_action(
            decoded->input, decoded->input_root_digest, decoded->sandbox, *hash_algorithm);
        auto encoded_action =
            messages ? reapi::encode(messages->action)
                     : result<std::vector<std::byte>>{std::unexpected(messages.error())};
        auto encoded_command =
            messages ? reapi::encode(messages->command)
                     : result<std::vector<std::byte>>{std::unexpected(messages.error())};
        if (!encoded_action || !encoded_command) {
            co_return std::unexpected(!encoded_action ? encoded_action.error()
                                                      : encoded_command.error());
        }
        queued_action = std::move(*encoded_action);
        queued_command = std::move(*encoded_command);
    }
    if (remote_eligible) {
        decoded->input.remote_eligible = *remote_eligible;
    }
    auto native_bytes = objects::encode(objects::action_record{decoded->input, decoded->sandbox});
    auto reconstructed = native_bytes ? objects::digest_of(decoded->input.inputs.alg, *native_bytes)
                                      : result<digest>{std::unexpected(native_bytes.error())};
    if (!reconstructed) {
        co_return std::unexpected(reconstructed.error());
    }
    if (native_action && (native_action->alg != reconstructed->alg ||
                          native_action->bytes != reconstructed->bytes)) {
        co_return std::unexpected(server_error(
            errc::corrupt, "native action does not match REAPI action", to_string(*native_action)));
    }
    co_return state.executions->submit(action_digest,
                                       effective_function,
                                       std::move(queued_action),
                                       std::move(queued_command),
                                       std::move(*decoded),
                                       *reconstructed,
                                       std::move(schedule));
}

runtime::task<void> handle_execute(http2::connection& connection,
                                   std::uint32_t stream,
                                   server_state& state,
                                   std::span<const std::byte> bytes) {
    auto request = reapi::decode_message<reapi::ExecuteRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance_name);
    if (!valid || !request->action_digest) {
        co_return co_await send_failure(
            connection,
            stream,
            !valid ? valid.error()
                   : server_error(errc::invalid_argument, "execution action digest is missing"));
    }
    auto schedule =
        execution_schedule{.decision = execution_decision::remote,
                           .inputs = {.size_class = action_size_class::medium},
                           .reason = "stock REAPI submission entered the scheduler queue"};
    auto operation = co_await submit_execution(
        state, *request->action_digest, request->digest_function, std::move(schedule));
    if (!operation) {
        co_return co_await send_failure(connection, stream, operation.error());
    }
    co_return co_await send_message(connection, stream, *operation);
}

runtime::task<void> handle_submit(http2::connection& connection,
                                  std::uint32_t stream,
                                  server_state& state,
                                  std::span<const std::byte> bytes) {
    auto request = workers::decode<workers::submit_request>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance);
    if (!valid) {
        co_return co_await send_failure(connection, stream, valid.error());
    }
    auto operation = co_await submit_execution(state,
                                               request->action,
                                               reapi::DigestFunctionValue::blake3,
                                               std::move(request->schedule),
                                               request->native_action,
                                               request->remote_eligible);
    if (!operation) {
        co_return co_await send_failure(connection, stream, operation.error());
    }
    co_return co_await send_message(connection, stream, *operation);
}

runtime::task<void> handle_wait(http2::connection& connection,
                                std::uint32_t stream,
                                server_state& state,
                                std::span<const std::byte> bytes) {
    if (!state.executions) {
        co_return co_await send_status(connection, stream, 12, "REAPI execution is disabled");
    }
    auto request = reapi::decode_message<reapi::WaitExecutionRequest>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    for (;;) {
        auto operation = state.executions->operation(request->name);
        if (!operation) {
            co_return co_await send_failure(connection, stream, operation.error());
        }
        if (operation->done) {
            co_return co_await send_message(connection, stream, *operation);
        }
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{25});
        if (delay_error) {
            co_return std::unexpected(
                server_error(errc::unavailable, delay_error.message(), request->name));
        }
    }
}

runtime::task<void> handle_cancel(http2::connection& connection,
                                  std::uint32_t stream,
                                  server_state& state,
                                  std::span<const std::byte> bytes) {
    if (!state.executions) {
        co_return co_await send_status(connection, stream, 12, "REAPI execution is disabled");
    }
    auto request = reapi::decode_message<reapi::CancelOperationRequest>(bytes);
    auto cancelled = request ? state.executions->cancel(request->name)
                             : result<void>{std::unexpected(request.error())};
    if (!cancelled) {
        co_return co_await send_failure(connection, stream, cancelled.error());
    }
    co_return co_await send_message(connection, stream, reapi::Empty{});
}

runtime::task<void> handle_lease(http2::connection& connection,
                                 std::uint32_t stream,
                                 server_state& state,
                                 std::span<const std::byte> bytes) {
    if (!state.executions) {
        co_return co_await send_status(connection, stream, 12, "worker scheduling is disabled");
    }
    auto request = workers::decode<workers::worker_request>(bytes);
    if (!request) {
        co_return co_await send_failure(connection, stream, request.error());
    }
    auto valid = validate_instance(state.instance, request->instance);
    auto leased = valid ? state.executions->lease(*request)
                        : result<workers::lease_response>{std::unexpected(valid.error())};
    if (!leased) {
        co_return co_await send_failure(connection, stream, leased.error());
    }
    if (!state.lease_signer) {
        co_return co_await send_failure(
            connection, stream, server_error(errc::unauthorized, "scheduler has no lease signer"));
    }
    auto body = workers::signing_body(*leased);
    if (!body) {
        co_return co_await send_failure(connection, stream, body.error());
    }
    auto signed_lease =
        co_await state.lease_signer->sign(std::string{workers::lease_purpose}, std::move(*body));
    auto signature = signed_lease
                         ? identity_detail::canonical_signature(*signed_lease)
                         : result<std::vector<std::byte>>{std::unexpected(signed_lease.error())};
    if (!signature) {
        co_return co_await send_failure(connection, stream, signature.error());
    }
    leased->signature = std::move(*signature);
    co_return co_await send_worker_message(connection, stream, *leased);
}

runtime::task<void> handle_heartbeat(http2::connection& connection,
                                     std::uint32_t stream,
                                     server_state& state,
                                     std::span<const std::byte> bytes) {
    auto request = workers::decode<workers::heartbeat_request>(bytes);
    if (!request || !state.executions) {
        co_return co_await send_failure(
            connection,
            stream,
            request ? server_error(errc::unsupported, "worker scheduling is disabled")
                    : request.error());
    }
    auto acknowledged = state.executions->heartbeat(*request);
    if (!acknowledged) {
        co_return co_await send_failure(connection, stream, acknowledged.error());
    }
    co_return co_await send_worker_message(connection, stream, *acknowledged);
}

runtime::task<void> handle_complete(http2::connection& connection,
                                    std::uint32_t stream,
                                    server_state& state,
                                    std::span<const std::byte> bytes) {
    auto request = workers::decode<workers::complete_request>(bytes);
    if (!request || !state.executions) {
        co_return co_await send_failure(
            connection,
            stream,
            request ? server_error(errc::unsupported, "worker scheduling is disabled")
                    : request.error());
    }
    if (!request->failure.empty()) {
        auto failed = state.executions->fail(*request);
        if (!failed) {
            co_return co_await send_failure(connection, stream, failed.error());
        }
        co_return co_await send_worker_message(connection, stream, *failed);
    }
    auto result = reapi::decode_message<reapi::ActionResult>(request->result);
    if (!result) {
        co_return co_await send_failure(connection, stream, result.error());
    }
    auto completed = state.executions->complete(*request, *result);
    if (!completed) {
        co_return co_await send_failure(connection, stream, completed.error());
    }
    const auto& accepted = *completed;
    if (accepted.accepted) {
        auto action = state.executions->action_digest(request->operation);
        auto encoded = reapi::encode(*result);
        if (!action || !encoded) {
            co_return co_await send_failure(
                connection, stream, !action ? action.error() : encoded.error());
        }
        // Validated against its digest function at submission; both supported functions are
        // 256-bit, so the length check is the same for either.
        auto entry = cache_entry(*action, reapi::DigestFunctionValue::unknown);
        if (!entry) {
            co_return co_await send_failure(connection, stream, entry.error());
        }
        auto written = reapi::write_action_cache(state.action_cache, *entry, *encoded);
        if (!written) {
            co_return co_await send_failure(connection, stream, written.error());
        }
    }
    co_return co_await send_worker_message(connection, stream, accepted);
}

runtime::task<void> handle_read_object(http2::connection& connection,
                                       std::uint32_t stream,
                                       server_state& state,
                                       std::span<const std::byte> bytes) {
    auto request = workers::decode<workers::read_object_request>(bytes);
    auto id =
        request ? parse_digest(request->digest) : result<digest>{std::unexpected(request.error())};
    auto contents = id ? co_await state.objects.get(*id)
                       : result<std::vector<std::byte>>{std::unexpected(id.error())};
    if (!contents) {
        co_return co_await send_failure(connection, stream, contents.error());
    }
    co_return co_await send_worker_message(
        connection, stream, workers::read_object_response{std::move(*contents)});
}

runtime::task<void>
handle_scheduler_status(http2::connection& connection, std::uint32_t stream, server_state& state) {
    if (!state.executions) {
        co_return co_await send_status(connection, stream, 12, "worker scheduling is disabled");
    }
    co_return co_await send_worker_message(connection, stream, state.executions->status());
}

runtime::task<void> handle_scheduler_operation(http2::connection& connection,
                                               std::uint32_t stream,
                                               server_state& state,
                                               std::span<const std::byte> bytes) {
    if (!state.executions) {
        co_return co_await send_status(connection, stream, 12, "worker scheduling is disabled");
    }
    auto request = reapi::decode_message<reapi::WaitExecutionRequest>(bytes);
    auto operation = request ? state.executions->operation(request->name)
                             : result<reapi::Operation>{std::unexpected(request.error())};
    if (!operation) {
        co_return co_await send_failure(connection, stream, operation.error());
    }
    co_return co_await send_message(connection, stream, *operation);
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
    if (method == execute_method) {
        co_return co_await handle_execute(connection, stream, state, bytes);
    }
    if (method == wait_method) {
        co_return co_await handle_wait(connection, stream, state, bytes);
    }
    if (method == cancel_method) {
        co_return co_await handle_cancel(connection, stream, state, bytes);
    }
    if (method == workers::submit_method) {
        co_return co_await handle_submit(connection, stream, state, bytes);
    }
    if (method == workers::lease_method) {
        co_return co_await handle_lease(connection, stream, state, bytes);
    }
    if (method == workers::heartbeat_method) {
        co_return co_await handle_heartbeat(connection, stream, state, bytes);
    }
    if (method == workers::complete_method) {
        co_return co_await handle_complete(connection, stream, state, bytes);
    }
    if (method == workers::read_object_method) {
        co_return co_await handle_read_object(connection, stream, state, bytes);
    }
    if (method == workers::status_method) {
        co_return co_await handle_scheduler_status(connection, stream, state);
    }
    if (method == workers::operation_method) {
        co_return co_await handle_scheduler_operation(connection, stream, state, bytes);
    }
    co_return co_await send_status(connection, stream, 12, "method is not implemented");
}

// gRPC UNAUTHENTICATED: the request carried no valid credentials.
constexpr auto unauthenticated_status = std::uint32_t{16};

auto authenticated(const server_state& state, std::span<const http2::header_field> headers)
    -> bool {
    if (state.bearer_token.empty()) {
        return true;
    }
    const auto authorization = header(headers, "authorization");
    return authorization && bearer_authorized(state.bearer_token, *authorization);
}

runtime::task<void> serve_connection(server_state& state, boost::capy::any_stream& stream) {
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
        if (!authenticated(state, *headers)) {
            // Refused before the request body is read; the connection is then closed, so an
            // unauthenticated peer can neither write nor make the server buffer anything.
            auto refused = co_await send_status(
                connection, *stream_id, unauthenticated_status, "missing or invalid bearer token");
            if (!refused) {
                co_return std::unexpected(refused.error());
            }
            co_return co_await connection.goaway();
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
        // The body is never read whole (liquid#451): a Write is applied one message at a time
        // and a unary request is one message, each within its limit.
        auto request = request_stream{connection,
                                      *stream_id,
                                      *method == write_method ? max_write_message_bytes
                                                              : max_unary_message_bytes};
        if (*method == write_method) {
            auto handled = co_await handle_write(connection, *stream_id, state, request);
            if (!handled) {
                co_return std::unexpected(handled.error());
            }
        } else {
            auto message = co_await request_message(request);
            if (!message) {
                auto failed =
                    request.exceeded()
                        ? co_await send_status(connection,
                                               *stream_id,
                                               8,
                                               std::format("request message exceeds {} bytes",
                                                           request.limit()))
                        : co_await send_failure(connection, *stream_id, message.error());
                if (!failed) {
                    co_return std::unexpected(failed.error());
                }
            } else {
                auto handled = co_await dispatch(connection, *stream_id, state, *method, *message);
                if (!handled) {
                    co_return std::unexpected(handled.error());
                }
            }
        }
        // A stream answered before the peer finished sending it (a refused or malformed request)
        // still has bytes on the way. Reading them would hold them; leaving them would queue
        // them. Close the connection instead.
        if (!request.finished()) {
            auto closed = co_await connection.goaway();
            if (!closed) {
                co_return std::unexpected(closed.error());
            }
            co_return {};
        }
    }
}

// How long, and how many bytes, a closing connection reads and discards from its peer.
constexpr auto linger_timeout = std::chrono::seconds{2};
constexpr auto linger_bytes = std::size_t{16} * 1024 * 1024;

runtime::task<void> discard_until_peer_closes(boost::corosio::tcp_socket& socket) {
    auto buffer = std::array<std::byte, 16 * 1024>{};
    for (auto discarded = std::size_t{}; discarded < linger_bytes;) {
        const auto [read_error, count] =
            co_await socket.read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
        if (read_error || count == 0) {
            break;
        }
        discarded += count;
    }
    co_return result<void>{};
}

// Ends a connection without resetting it (liquid#548). Closing a TCP socket that still holds
// received bytes it has not read makes the kernel send a reset, and a reset discards whatever the
// peer has not read yet. A request answered before its body arrived (refused from its prefix, or
// unauthenticated) leaves that body on the way, so on Linux its answer and GOAWAY were lost and
// the client saw no response. The server ends its side with FIN, then reads and discards what
// still arrives until the peer closes, within `linger_timeout` and `linger_bytes`.
runtime::task<void> close_gracefully(boost::corosio::tcp_socket& socket) {
    socket.shutdown(boost::corosio::tcp_socket::shutdown_send);
    auto [deadline_error, outcome] = co_await runtime::timeout(
        runtime::as_io(discard_until_peer_closes(socket)), linger_timeout);
    static_cast<void>(deadline_error);
    static_cast<void>(outcome);
    co_return result<void>{};
}

runtime::task<void> session(std::shared_ptr<server_state> state,
                            std::shared_ptr<boost::corosio::tcp_socket> socket,
                            std::shared_ptr<boost::corosio::tls_context> tls) {
    if (!tls) {
        auto stream = boost::capy::any_stream{socket.get()};
        auto served = co_await serve_connection(*state, stream);
        static_cast<void>(co_await close_gracefully(*socket));
        co_return served;
    }
#if defined(BOOST_COROSIO_HAS_OPENSSL)
    auto secure = boost::corosio::openssl_stream{socket.get(), *tls};
    auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::server);
    if (handshake_error) {
        co_return std::unexpected(
            server_error(errc::unauthorized, handshake_error.message(), "REAPI TLS handshake"));
    }
    auto negotiated = http2::validate_tls_protocol(secure);
    if (!negotiated) {
        co_return std::unexpected(negotiated.error());
    }
    auto stream = boost::capy::any_stream{&secure};
    auto served = co_await serve_connection(*state, stream);
    static_cast<void>(co_await close_gracefully(*socket));
    co_return served;
#else
    co_return std::unexpected(
        server_error(errc::unsupported, "TLS support is unavailable in this build", "REAPI"));
#endif
}

// The TLS context of a listener: its certificate chain and key, HTTP/2 by ALPN, and, with a client
// certificate authority, a required client certificate that authority signs (mutual TLS).
auto make_tls_context(const listener_security& security)
    -> result<std::shared_ptr<boost::corosio::tls_context>> {
    if (!security.tls_certificate || !security.tls_key) {
        return std::shared_ptr<boost::corosio::tls_context>{};
    }
#if defined(BOOST_COROSIO_HAS_OPENSSL)
    auto tls = std::make_shared<boost::corosio::tls_context>();
    const auto certificate = security.tls_certificate->string();
    if (const auto code = tls->use_certificate_chain_file(certificate)) {
        return std::unexpected(server_error(errc::invalid_argument, code.message(), certificate));
    }
    const auto key = security.tls_key->string();
    if (const auto code = tls->use_private_key_file(key, boost::corosio::tls_file_format::pem)) {
        return std::unexpected(server_error(errc::invalid_argument, code.message(), key));
    }
    if (const auto code = tls->set_alpn({"h2"})) {
        return std::unexpected(server_error(errc::io, code.message(), "REAPI TLS ALPN"));
    }
    if (security.client_certificate_authority) {
        const auto authority = security.client_certificate_authority->string();
        if (const auto code = tls->load_verify_file(authority)) {
            return std::unexpected(server_error(errc::invalid_argument, code.message(), authority));
        }
        if (const auto code = tls->set_verify_mode(boost::corosio::tls_verify_mode::require_peer)) {
            return std::unexpected(server_error(errc::io, code.message(), "REAPI client TLS"));
        }
    }
    return tls;
#else
    return std::unexpected(
        server_error(errc::unsupported, "TLS support is unavailable in this build", "REAPI"));
#endif
}

} // namespace

runtime::task<void> serve_reapi_impl(reapi_serve_options options,
                                     std::shared_ptr<workers::scheduler> executions,
                                     std::shared_ptr<signer> lease_signer) {
    auto limits_valid = reapi::validate_upload_limits(options.uploads);
    if (!limits_valid) {
        co_return std::unexpected(limits_valid.error());
    }
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(options.listen, endpoint)) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, code.message(), options.listen));
    }
    if (endpoint.port() == 0) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "REAPI listen port must be non-zero"));
    }
    auto tls = make_tls_context(options.security);
    if (!tls) {
        co_return std::unexpected(tls.error());
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
    auto state = std::make_shared<server_state>(std::move(*opened),
                                                std::move(action_cache),
                                                std::move(options.instance_name),
                                                std::move(executions),
                                                std::move(lease_signer),
                                                options.uploads);
    state->bearer_token = std::move(options.security.bearer_token);
    const auto executor = co_await boost::capy::this_coro::executor;
    auto acceptor = boost::corosio::tcp_acceptor{executor.context(), endpoint};
    for (;;) {
        auto socket = std::make_shared<boost::corosio::tcp_socket>(executor.context());
        const auto [accept_error] = co_await acceptor.accept(*socket);
        if (accept_error) {
            co_return std::unexpected(
                server_error(errc::io, accept_error.message(), options.listen));
        }
        runtime::spawn(session(state, std::move(socket), *tls), [](result<void> outcome) {
            if (!outcome && outcome.error().message != "End of file" &&
                outcome.error().message != "eof" &&
                outcome.error().message != "peer has sent GOAWAY") {
                std::println(stderr,
                             "REAPI connection error: {} ({})",
                             outcome.error().message,
                             outcome.error().context);
            }
        });
    }
}

namespace {

// Refuses a listener on a non-loopback address unless it has TLS and a bearer token or a client
// certificate authority (liquid#415). Shared by `serve_reapi` and `serve_scheduler` (liquid#520),
// and run before anything is opened or bound.
auto check_listener(const std::string& listen, const listener_security& security) -> result<void> {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(listen, endpoint)) {
        return std::unexpected(server_error(errc::invalid_argument, code.message(), listen));
    }
    const auto loopback = endpoint.is_v4() ? endpoint.v4_address().is_loopback()
                                           : endpoint.v6_address().is_loopback();
    auto secured = validate_listener_security(loopback, security);
    if (!secured) {
        auto refused = secured.error();
        refused.context = listen + ": " + refused.context;
        return std::unexpected(std::move(refused));
    }
    return {};
}

} // namespace

runtime::task<void> serve_reapi(reapi_serve_options options) {
    auto secured = check_listener(options.listen, options.security);
    if (!secured) {
        co_return std::unexpected(secured.error());
    }
    co_return co_await serve_reapi_impl(std::move(options), {}, {});
}

runtime::task<void> serve_scheduler(scheduler_options options) {
    if (options.lease_duration <= std::chrono::milliseconds{}) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "scheduler lease duration must be positive"));
    }
    // The scheduler serves the same CAS, ActionCache and Execution services as `serve_reapi`, plus
    // the worker lease queue, so it gets the same listener rules and per-RPC bearer check
    // (liquid#520). That transport authentication decides who may reach the scheduler at all; it
    // does not replace the pinned worker identities below, whose signatures every lease and every
    // result still carry (liquid#416).
    auto secured = check_listener(options.listen, options.security);
    if (!secured) {
        co_return std::unexpected(secured.error());
    }
    if (options.trusted_workers.empty()) {
        co_return std::unexpected(server_error(
            errc::invalid_argument,
            "scheduler has no trusted workers",
            "pass each worker's `liquid identity show --document` with --trust-worker"));
    }
    auto trusted = std::vector<actor>{};
    for (const auto& document : options.trusted_workers) {
        auto key = workers::pinned_key(document);
        if (!key) {
            co_return std::unexpected(key.error());
        }
        trusted.push_back(std::move(*key));
    }
    auto identity = co_await identity_detail::local_signer();
    if (!identity) {
        co_return std::unexpected(identity.error());
    }
    auto lease_signer = std::make_shared<signer>(std::move(*identity));
    auto executions =
        std::make_shared<workers::scheduler>(options.lease_duration, std::move(trusted));
    runtime::spawn(
        workers::serve_control(
            std::move(options.control_listen),
            workers::control_status{.component = "scheduler",
                                    .ready = [executions] { return executions->ready(); },
                                    .metrics =
                                        [executions] {
                                            const auto status = executions->status();
                                            return std::format("liquid_scheduler_queued {}\n"
                                                               "liquid_scheduler_in_flight {}\n"
                                                               "liquid_scheduler_completed {}\n"
                                                               "liquid_scheduler_failed {}\n"
                                                               "liquid_scheduler_requeued {}\n"
                                                               "liquid_scheduler_workers {}\n",
                                                               status.queued,
                                                               status.in_flight,
                                                               status.completed,
                                                               status.failed,
                                                               status.requeued,
                                                               status.workers);
                                        }}),
        [](result<void> outcome) {
            if (!outcome) {
                std::println(stderr,
                             "scheduler control server error: {} ({})",
                             outcome.error().message,
                             outcome.error().context);
            }
        });
    auto reapi_options = reapi_serve_options{.store_directory = std::move(options.store_directory),
                                             .listen = std::move(options.listen),
                                             .instance_name = std::move(options.instance_name),
                                             .uploads = options.uploads,
                                             .security = std::move(options.security)};
    co_return co_await serve_reapi_impl(
        std::move(reapi_options), std::move(executions), std::move(lease_signer));
}

} // namespace liquid
