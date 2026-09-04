#include "reapi/remote_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "liquid/build.hpp"
#include "liquid/object.hpp"
#include "objects/records.hpp"
#include "reapi/conversion.hpp"
#include "workers/protocol.hpp"

namespace liquid::reapi {
namespace {

constexpr auto capabilities_method =
    "/build.bazel.remote.execution.v2.Capabilities/GetCapabilities";
constexpr auto find_missing_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/FindMissingBlobs";
constexpr auto batch_read_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchReadBlobs";
constexpr auto batch_update_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchUpdateBlobs";
constexpr auto read_method = "/google.bytestream.ByteStream/Read";
constexpr auto write_method = "/google.bytestream.ByteStream/Write";
constexpr auto query_write_method = "/google.bytestream.ByteStream/QueryWriteStatus";
constexpr auto execute_method = "/build.bazel.remote.execution.v2.Execution/Execute";
constexpr auto wait_method = "/build.bazel.remote.execution.v2.Execution/WaitExecution";
constexpr auto cancel_method = "/google.longrunning.Operations/CancelOperation";
constexpr auto get_action_result_method =
    "/build.bazel.remote.execution.v2.ActionCache/GetActionResult";
constexpr auto update_action_result_method =
    "/build.bazel.remote.execution.v2.ActionCache/UpdateActionResult";

auto client_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

template <typename Request, typename Response>
runtime::task<Response>
unary(grpc_client& client, std::string_view method, const Request& request) {
    auto encoded = encode(request);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto messages = co_await client.call(method, *encoded);
    if (!messages) {
        co_return std::unexpected(messages.error());
    }
    if (messages->size() != 1) {
        co_return std::unexpected(client_error(
            errc::corrupt, "unary gRPC response has the wrong message count", std::string{method}));
    }
    auto output = decode_message<Response>(messages->front());
    if (!output) {
        co_return std::unexpected(output.error());
    }
    co_return std::move(*output);
}

template <typename Request, typename Response>
runtime::task<std::vector<Response>>
streaming(grpc_client& client, std::string_view method, const Request& request) {
    auto encoded = encode(request);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto messages = co_await client.call(method, *encoded, true);
    if (!messages) {
        co_return std::unexpected(messages.error());
    }
    auto output = std::vector<Response>{};
    output.reserve(messages->size());
    for (const auto& message : *messages) {
        auto decoded = decode_message<Response>(message);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        output.push_back(std::move(*decoded));
    }
    co_return output;
}

auto instance_prefix(std::string_view instance) -> std::string {
    return instance.empty() ? std::string{} : std::string{instance} + "/";
}

auto read_resource(std::string_view instance, const Digest& blob) -> std::string {
    return instance_prefix(instance) + "blobs/" + blob.hash + "/" + std::to_string(blob.size_bytes);
}

auto write_resource(std::string_view instance, const Digest& blob) -> std::string {
    return instance_prefix(instance) + "uploads/liquid-" + blob.hash + "/blobs/" + blob.hash + "/" +
           std::to_string(blob.size_bytes);
}

auto status_ok(const std::optional<Status>& status, std::string_view context) -> result<void> {
    if (!status || status->code == 0) {
        return {};
    }
    return std::unexpected(
        client_error(status->code == 5 ? errc::not_found : errc::unavailable,
                     status->message.empty() ? "REAPI request failed" : status->message,
                     std::string{context}));
}

auto digest_key(const Digest& value) -> std::string {
    return value.hash + "/" + std::to_string(value.size_bytes);
}

auto action_cache_digest(repository& source,
                         const action_input& input,
                         digest sandbox,
                         algorithm wire_algorithm) -> result<Digest> {
    auto cache = build_detail::disk_cache{source};
    auto candidate = cache.candidate_key(input, sandbox);
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    auto bytes = std::vector<std::byte>{std::byte{static_cast<unsigned char>(candidate->alg)}};
    bytes.insert(bytes.end(), candidate->bytes.begin(), candidate->bytes.end());
    return make_digest(wire_algorithm, bytes);
}

auto staged_bytes(const std::map<std::string, std::vector<std::byte>>& staged, digest id)
    -> result<std::vector<std::byte>> {
    const auto found = staged.find(to_string(id));
    if (found == staged.end()) {
        return std::unexpected(
            client_error(errc::not_found, "prepared object is unavailable", to_string(id)));
    }
    return found->second;
}

auto staged_blob(const std::map<std::string, std::vector<std::byte>>& staged, digest id)
    -> result<std::vector<std::byte>> {
    auto record_bytes = staged_bytes(staged, id);
    if (!record_bytes) {
        return std::unexpected(record_bytes.error());
    }
    auto record = objects::decode_blob(*record_bytes);
    if (!record) {
        return std::unexpected(record.error());
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(record->size));
    for (const auto& chunk : record->chunks) {
        auto bytes = staged_bytes(staged, chunk);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != record->size) {
        return std::unexpected(
            client_error(errc::corrupt, "prepared blob has the wrong size", to_string(id)));
    }
    return output;
}

runtime::task<void>
stage_blob(repository& source, digest id, std::map<std::string, std::vector<std::byte>>& staged) {
    auto record_bytes = co_await repository_detail::build_access::get(source, id);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    auto record = objects::decode_blob(*record_bytes);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    staged.insert_or_assign(to_string(id), std::move(*record_bytes));
    for (const auto& chunk : record->chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        staged.insert_or_assign(to_string(chunk), std::move(*bytes));
    }
    co_return result<void>{};
}

runtime::task<void>
stage_tree(repository& source, digest id, std::map<std::string, std::vector<std::byte>>& staged) {
    if (staged.contains(to_string(id))) {
        co_return result<void>{};
    }
    auto tree_bytes = co_await repository_detail::build_access::get(source, id);
    if (!tree_bytes) {
        co_return std::unexpected(tree_bytes.error());
    }
    auto value = objects::decode_tree(*tree_bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    staged.insert_or_assign(to_string(id), std::move(*tree_bytes));
    for (const auto& entry : value->entries) {
        if (entry.kind == node_kind::tree) {
            auto nested = co_await stage_tree(source, entry.ref, staged);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            continue;
        }
        if (entry.kind == node_kind::blob) {
            auto blob = co_await stage_blob(source, entry.ref, staged);
            if (!blob) {
                co_return std::unexpected(blob.error());
            }
        }
    }
    co_return result<void>{};
}

auto operation_response(const Operation& operation) -> result<ExecuteResponse> {
    if (!operation.done) {
        return std::unexpected(client_error(
            errc::unavailable, "execution operation did not complete", operation.name));
    }
    if (const auto* status = std::get_if<Status>(&operation.result)) {
        return std::unexpected(
            client_error(status->code == 5 ? errc::not_found : errc::unavailable,
                         status->message.empty() ? "remote execution failed" : status->message,
                         operation.name));
    }
    const auto* response = std::get_if<Any>(&operation.result);
    if (!response) {
        return std::unexpected(
            client_error(errc::corrupt, "completed operation has no response", operation.name));
    }
    auto decoded = decode_message<ExecuteResponse>(response->value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto status = status_ok(decoded->status, operation.name);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (!decoded->result) {
        return std::unexpected(
            client_error(errc::corrupt, "ExecuteResponse has no action result", operation.name));
    }
    return std::move(*decoded);
}

} // namespace

struct remote_client::prepared_reapi_action {
    ActionMessages messages;
    std::vector<std::pair<Digest, std::vector<std::byte>>> blobs;
};

remote_client::remote_client(remote_config config) : rpc_(std::move(config)) {}

runtime::task<void> remote_client::initialize() {
    auto capabilities = co_await unary<GetCapabilitiesRequest, ServerCapabilities>(
        rpc_, capabilities_method, GetCapabilitiesRequest{.instance_name = instance()});
    if (!capabilities) {
        co_return std::unexpected(capabilities.error());
    }
    auto selected = select_digest_algorithm(*capabilities, false);
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    selected_ = *selected;
    co_return result<void>{};
}

runtime::task<void> remote_client::prepare(std::string, std::vector<remote_blob> blobs) {
    for (auto& blob : blobs) {
        auto alias = make_digest(selected_, blob.contents);
        if (!alias) {
            co_return std::unexpected(alias.error());
        }
        aliases_.insert_or_assign(to_string(blob.id), *alias);
        staged_.insert_or_assign(to_string(blob.id), std::move(blob.contents));
    }
    co_return result<void>{};
}

runtime::task<std::vector<digest>> remote_client::find_missing(std::string instance_name,
                                                               std::vector<digest> blobs_to_find) {
    if (rpc_.config().backend == remote_backend::liquid_scheduler) {
        for (const auto& blob : blobs_to_find) {
            if (!aliases_.contains(to_string(blob))) {
                co_return std::unexpected(client_error(
                    errc::invalid_argument, "remote blob was not prepared", to_string(blob)));
            }
        }
        co_return blobs_to_find;
    }
    auto request = FindMissingBlobsRequest{.instance_name = std::move(instance_name)};
    auto requested = std::map<std::string, digest>{};
    for (const auto& blob : blobs_to_find) {
        const auto alias = aliases_.find(to_string(blob));
        if (alias == aliases_.end()) {
            co_return std::unexpected(client_error(
                errc::invalid_argument, "remote blob was not prepared", to_string(blob)));
        }
        request.blob_digests.push_back(alias->second);
        requested.emplace(digest_key(alias->second), blob);
    }
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    request.digest_function = *function;
    auto response = co_await unary<FindMissingBlobsRequest, FindMissingBlobsResponse>(
        rpc_, find_missing_method, request);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto output = std::vector<digest>{};
    for (const auto& missing : response->missing_blob_digests) {
        const auto found = requested.find(digest_key(missing));
        if (found == requested.end()) {
            co_return std::unexpected(client_error(
                errc::corrupt, "peer returned an unrequested missing digest", missing.hash));
        }
        output.push_back(found->second);
    }
    co_return output;
}

runtime::task<std::vector<std::byte>> remote_client::read_blob(const Digest& blob) {
    if (blob.size_bytes < 0) {
        co_return std::unexpected(
            client_error(errc::corrupt, "peer returned a negative blob size", blob.hash));
    }
    if (static_cast<std::uint64_t>(blob.size_bytes) <= rpc_.config().batch_blob_limit) {
        auto function = digest_function(selected_);
        if (!function) {
            co_return std::unexpected(function.error());
        }
        auto response = co_await unary<BatchReadBlobsRequest, BatchReadBlobsResponse>(
            rpc_,
            batch_read_method,
            BatchReadBlobsRequest{
                .instance_name = instance(), .digests = {blob}, .digest_function = *function});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (response->responses.size() != 1 || !response->responses.front().digest) {
            co_return std::unexpected(
                client_error(errc::corrupt, "BatchReadBlobs returned no matching blob", blob.hash));
        }
        auto status = status_ok(response->responses.front().status, blob.hash);
        if (!status) {
            co_return std::unexpected(status.error());
        }
        co_return std::move(response->responses.front().data);
    }
    auto responses = co_await streaming<ReadRequest, ReadResponse>(
        rpc_, read_method, ReadRequest{.resource_name = read_resource(instance(), blob)});
    if (!responses) {
        co_return std::unexpected(responses.error());
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(blob.size_bytes));
    for (auto& response : *responses) {
        output.insert(output.end(), response.data.begin(), response.data.end());
    }
    co_return output;
}

runtime::task<std::uint64_t> remote_client::read_range(std::string,
                                                       digest blob,
                                                       std::uint64_t offset,
                                                       std::span<std::byte> output) {
    const auto staged = staged_.find(to_string(blob));
    if (staged != staged_.end()) {
        if (offset >= staged->second.size()) {
            co_return std::uint64_t{};
        }
        const auto count = std::min<std::size_t>(output.size(), staged->second.size() - offset);
        std::ranges::copy_n(
            staged->second.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        co_return count;
    }
    const auto alias = aliases_.find(to_string(blob));
    if (alias == aliases_.end()) {
        if (rpc_.config().backend != remote_backend::liquid_scheduler) {
            co_return std::unexpected(client_error(
                errc::not_found, "remote digest alias is unavailable", to_string(blob)));
        }
        auto request = workers::encode(workers::read_object_request{to_string(blob)});
        if (!request) {
            co_return std::unexpected(request.error());
        }
        auto messages = co_await rpc_.call(workers::read_object_method, *request);
        if (!messages) {
            co_return std::unexpected(messages.error());
        }
        if (messages->size() != 1) {
            co_return std::unexpected(
                client_error(errc::corrupt, "ReadObject returned the wrong response count"));
        }
        auto response = workers::decode<workers::read_object_response>(messages->front());
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto actual = hash(blob.alg, response->contents);
        if (!actual || actual->bytes != blob.bytes) {
            co_return std::unexpected(
                actual ? client_error(errc::corrupt, "ReadObject digest changed", to_string(blob))
                       : actual.error());
        }
        staged_.insert_or_assign(to_string(blob), std::move(response->contents));
        co_return co_await read_range({}, blob, offset, output);
    }
    auto bytes = co_await read_blob(alias->second);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    if (offset >= bytes->size()) {
        co_return std::uint64_t{};
    }
    const auto count = std::min<std::size_t>(output.size(), bytes->size() - offset);
    std::ranges::copy_n(
        bytes->begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
    co_return count;
}

runtime::task<void>
remote_client::write(std::string instance_name, digest blob, std::vector<std::byte> contents) {
    auto alias = make_digest(selected_, contents);
    if (!alias) {
        co_return std::unexpected(alias.error());
    }
    aliases_.insert_or_assign(to_string(blob), *alias);
    staged_.insert_or_assign(to_string(blob), contents);
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    if (contents.size() <= rpc_.config().batch_blob_limit) {
        auto response = co_await unary<BatchUpdateBlobsRequest, BatchUpdateBlobsResponse>(
            rpc_,
            batch_update_method,
            BatchUpdateBlobsRequest{.instance_name = std::move(instance_name),
                                    .requests = {{.digest = *alias, .data = std::move(contents)}},
                                    .digest_function = *function});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (response->responses.size() != 1) {
            co_return std::unexpected(client_error(
                errc::corrupt, "BatchUpdateBlobs returned the wrong response count", alias->hash));
        }
        auto status = status_ok(response->responses.front().status, alias->hash);
        if (!status) {
            co_return std::unexpected(status.error());
        }
        co_return result<void>{};
    }
    const auto resource = write_resource(instance_name, *alias);
    auto offset = std::int64_t{};
    auto queried = co_await unary<QueryWriteStatusRequest, QueryWriteStatusResponse>(
        rpc_, query_write_method, QueryWriteStatusRequest{.resource_name = resource});
    if (queried) {
        if (queried->complete && queried->committed_size == alias->size_bytes) {
            co_return result<void>{};
        }
        if (queried->committed_size < 0 || queried->committed_size > alias->size_bytes) {
            co_return std::unexpected(client_error(
                errc::corrupt, "QueryWriteStatus returned an invalid offset", resource));
        }
        offset = queried->committed_size;
    } else if (queried.error().code != errc::not_found) {
        co_return std::unexpected(queried.error());
    }
    auto response = co_await unary<WriteRequest, WriteResponse>(
        rpc_,
        write_method,
        WriteRequest{.resource_name = resource,
                     .write_offset = offset,
                     .finish_write = true,
                     .data = std::vector<std::byte>{
                         contents.begin() + static_cast<std::ptrdiff_t>(offset), contents.end()}});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->committed_size != alias->size_bytes) {
        co_return std::unexpected(
            client_error(errc::corrupt, "ByteStream write committed the wrong size", resource));
    }
    co_return result<void>{};
}

remote_blob_source& remote_client::blobs() noexcept {
    return *this;
}

std::string remote_client::instance() const {
    return rpc_.config().instance_name;
}

std::uint64_t remote_client::queue_depth() const noexcept {
    return 0;
}

bool remote_client::supports_batch() const noexcept {
    return false;
}

runtime::task<remote_client::prepared_reapi_action>
remote_client::prepare_action(const action_input& input, digest sandbox) {
    auto directories = DirectoryCache{};
    auto converted =
        directories.convert(input.inputs,
                            selected_,
                            TreeSource{
                                .tree = [this](digest id) -> result<tree> {
                                    auto bytes = staged_bytes(staged_, id);
                                    if (!bytes) {
                                        return std::unexpected(bytes.error());
                                    }
                                    return objects::decode_tree(*bytes);
                                },
                                .blob = [this](digest id) { return staged_blob(staged_, id); },
                            });
    if (!converted) {
        co_return std::unexpected(converted.error());
    }
    auto messages = to_reapi_action(input, converted->root_digest, sandbox, selected_);
    if (!messages) {
        co_return std::unexpected(messages.error());
    }
    auto output = prepared_reapi_action{.messages = std::move(*messages)};
    for (auto& directory : converted->objects) {
        output.blobs.emplace_back(directory.digest, std::move(directory.bytes));
    }
    auto command_bytes = encode(output.messages.command);
    auto action_bytes = encode(output.messages.action);
    if (!command_bytes) {
        co_return std::unexpected(command_bytes.error());
    }
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    output.blobs.emplace_back(output.messages.command_digest, std::move(*command_bytes));
    output.blobs.emplace_back(output.messages.action_digest, std::move(*action_bytes));
    co_return output;
}

runtime::task<void>
remote_client::upload_prepared(const remote_client::prepared_reapi_action& prepared) {
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    auto request =
        FindMissingBlobsRequest{.instance_name = instance(), .digest_function = *function};
    for (const auto& [id, bytes] : prepared.blobs) {
        request.blob_digests.push_back(id);
    }
    auto missing = co_await unary<FindMissingBlobsRequest, FindMissingBlobsResponse>(
        rpc_, find_missing_method, request);
    if (!missing) {
        co_return std::unexpected(missing.error());
    }
    auto missing_keys = std::set<std::string>{};
    for (const auto& id : missing->missing_blob_digests) {
        missing_keys.insert(digest_key(id));
    }
    for (const auto& [id, bytes] : prepared.blobs) {
        if (!missing_keys.contains(digest_key(id))) {
            continue;
        }
        auto native = to_liquid_digest(id, selected_);
        if (!native) {
            co_return std::unexpected(native.error());
        }
        auto uploaded = co_await write(instance(), *native, bytes);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
    }
    co_return result<void>{};
}

runtime::task<action_result>
remote_client::native_result(const ActionResult& value, digest action, bool validate_action) {
    auto output = from_reapi_result(value, action, selected_, validate_action);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    if (rpc_.config().backend == remote_backend::liquid_scheduler) {
        co_return std::move(*output);
    }
    const auto make_record = [this,
                              action](const Digest& raw,
                                      std::span<const std::byte> inline_bytes) -> result<digest> {
        auto raw_id = to_liquid_digest(raw, selected_);
        if (!raw_id) {
            return std::unexpected(raw_id.error());
        }
        aliases_.insert_or_assign(to_string(*raw_id), raw);
        if (!inline_bytes.empty() || raw.size_bytes == 0) {
            staged_.insert_or_assign(
                to_string(*raw_id),
                std::vector<std::byte>{inline_bytes.begin(), inline_bytes.end()});
        }
        auto record = blob{.chunks = {*raw_id},
                           .size = static_cast<std::uint64_t>(raw.size_bytes),
                           .chunk_sizes = {static_cast<std::uint64_t>(raw.size_bytes)},
                           .content = *raw_id,
                           .remote = remote_blob_location{.source = rpc_.config().endpoint,
                                                          .instance = instance()}};
        auto encoded = objects::encode(record);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto record_id = objects::digest_of(action.alg, *encoded);
        if (!record_id) {
            return std::unexpected(record_id.error());
        }
        staged_.insert_or_assign(to_string(*record_id), std::move(*encoded));
        return *record_id;
    };
    output->outputs.clear();
    for (const auto& file : value.output_files) {
        if (!file.digest) {
            co_return std::unexpected(
                client_error(errc::corrupt, "remote output file has no digest", file.path));
        }
        auto record = make_record(*file.digest, file.contents);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        const auto fallback = file.is_executable ? 0755U : 0644U;
        auto mode = fallback;
        if (file.node_properties && file.node_properties->unix_mode) {
            mode = file.node_properties->unix_mode->value & 07777U;
        }
        output->outputs.push_back({.path = file.path, .mode = mode, .blob = *record});
    }
    const auto stream_record = [&](const std::optional<Digest>& id,
                                   std::span<const std::byte> bytes) -> result<digest> {
        if (id) {
            return make_record(*id, bytes);
        }
        auto made = make_digest(selected_, bytes);
        if (!made) {
            return std::unexpected(made.error());
        }
        return make_record(*made, bytes);
    };
    auto stdout_record = stream_record(value.stdout_digest, value.stdout_raw);
    auto stderr_record = stream_record(value.stderr_digest, value.stderr_raw);
    if (!stdout_record) {
        co_return std::unexpected(stdout_record.error());
    }
    if (!stderr_record) {
        co_return std::unexpected(stderr_record.error());
    }
    output->stdout_output = *stdout_record;
    output->stderr_output = *stderr_record;
    std::ranges::sort(output->outputs, {}, &action_output::path);
    co_return std::move(*output);
}

runtime::task<action_result> remote_client::execute_operation(
    action_input input, digest action, digest sandbox, std::optional<execution_schedule> schedule) {
    auto prepared = co_await prepare_action(input, sandbox);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto uploaded = co_await upload_prepared(*prepared);
    if (!uploaded) {
        co_return std::unexpected(uploaded.error());
    }
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    auto operations = result<std::vector<Operation>>{};
    if (rpc_.config().backend == remote_backend::liquid_scheduler) {
        if (!schedule) {
            co_return std::unexpected(
                client_error(errc::invalid_argument, "scheduler execution has no cost decision"));
        }
        auto request =
            workers::encode(workers::submit_request{.instance = instance(),
                                                    .action = prepared->messages.action_digest,
                                                    .schedule = *schedule,
                                                    .native_action = action,
                                                    .remote_eligible = input.remote_eligible});
        if (!request) {
            co_return std::unexpected(request.error());
        }
        auto response = co_await rpc_.call(workers::submit_method, *request);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        operations = std::vector<Operation>{};
        for (const auto& message : *response) {
            auto decoded = decode_message<Operation>(message);
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            operations->push_back(std::move(*decoded));
        }
    } else {
        operations = co_await streaming<ExecuteRequest, Operation>(
            rpc_,
            execute_method,
            ExecuteRequest{.instance_name = instance(),
                           .action_digest = prepared->messages.action_digest,
                           .digest_function = *function});
    }
    if (!operations) {
        co_return std::unexpected(operations.error());
    }
    if (operations->empty()) {
        co_return std::unexpected(client_error(errc::corrupt, "Execute returned no operation"));
    }
    auto operation = std::move(operations->back());
    if (!operation.done) {
        auto waited = co_await streaming<WaitExecutionRequest, Operation>(
            rpc_, wait_method, WaitExecutionRequest{.name = operation.name});
        if (!waited) {
            if (!operation.name.empty()) {
                static_cast<void>(co_await unary<CancelOperationRequest, Empty>(
                    rpc_, cancel_method, CancelOperationRequest{.name = operation.name}));
            }
            co_return std::unexpected(waited.error());
        }
        if (waited->empty()) {
            co_return std::unexpected(
                client_error(errc::corrupt, "WaitExecution returned no operation", operation.name));
        }
        operation = std::move(waited->back());
    }
    auto response = operation_response(operation);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    co_return co_await native_result(*response->result, action);
}

runtime::task<action_result> remote_client::execute(action_input input) {
    const auto empty = std::span<const std::byte>{};
    auto sandbox = hash(input.inputs.alg, empty);
    if (!sandbox) {
        co_return std::unexpected(sandbox.error());
    }
    auto native_action = objects::encode(objects::action_record{input, *sandbox});
    if (!native_action) {
        co_return std::unexpected(native_action.error());
    }
    auto action = objects::digest_of(input.inputs.alg, *native_action);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    co_return co_await execute_operation(std::move(input), *action, *sandbox, std::nullopt);
}

runtime::task<action_result> remote_client::execute_prepared(action_input input,
                                                             digest action,
                                                             digest sandbox,
                                                             execution_schedule schedule) {
    co_return co_await execute_operation(std::move(input), action, sandbox, std::move(schedule));
}

runtime::task<std::vector<action_result>>
remote_client::execute_batch(std::vector<action_input> inputs) {
    auto output = std::vector<action_result>{};
    output.reserve(inputs.size());
    for (auto& input : inputs) {
        auto value = co_await execute(std::move(input));
        if (!value) {
            co_return std::unexpected(value.error());
        }
        output.push_back(std::move(*value));
    }
    co_return output;
}

runtime::task<std::optional<action_result>>
remote_client::get_action_result(repository& source, action_input input, digest sandbox) {
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    auto cache_digest = action_cache_digest(source, input, sandbox, selected_);
    if (!cache_digest) {
        co_return std::unexpected(cache_digest.error());
    }
    auto response = co_await unary<GetActionResultRequest, ActionResult>(
        rpc_,
        get_action_result_method,
        GetActionResultRequest{.instance_name = instance(),
                               .action_digest = *cache_digest,
                               .digest_function = *function});
    if (!response) {
        if (response.error().code == errc::not_found) {
            co_return std::optional<action_result>{};
        }
        co_return std::unexpected(response.error());
    }
    auto native_action = objects::encode(objects::action_record{input, sandbox});
    if (!native_action) {
        co_return std::unexpected(native_action.error());
    }
    auto action_id = objects::digest_of(input.inputs.alg, *native_action);
    if (!action_id) {
        co_return std::unexpected(action_id.error());
    }
    auto native = co_await native_result(*response, *action_id, false);
    if (!native) {
        co_return std::unexpected(
            client_error(native.error().code, "get action result: " + native.error().message));
    }
    const auto persist = [&](digest id) -> runtime::task<void> {
        auto bytes = staged_bytes(staged_, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto record = objects::decode_blob(*bytes);
        if (!record) {
            co_return std::unexpected(
                client_error(record.error().code,
                             "remote output record: " + record.error().message,
                             to_string(id)));
        }
        for (const auto& chunk : record->chunks) {
            auto alias = aliases_.find(to_string(chunk));
            if (alias == aliases_.end()) {
                co_return std::unexpected(client_error(
                    errc::not_found, "remote output digest is unavailable", to_string(chunk)));
            }
            auto contents = co_await read_blob(alias->second);
            if (!contents) {
                co_return std::unexpected(contents.error());
            }
            auto raw = co_await repository_detail::build_access::put_record(
                source, std::move(*contents), chunk.alg);
            if (!raw || raw->alg != chunk.alg || raw->bytes != chunk.bytes) {
                co_return std::unexpected(
                    raw ? client_error(errc::corrupt,
                                       "downloaded remote blob digest changed",
                                       to_string(chunk))
                        : raw.error());
            }
        }
        auto stored =
            co_await repository_detail::build_access::put_record(source, std::move(*bytes), id.alg);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (stored->alg != id.alg || stored->bytes != id.bytes) {
            co_return std::unexpected(
                client_error(errc::corrupt, "stored remote record digest changed", to_string(id)));
        }
        co_return result<void>{};
    };
    for (const auto& item : native->outputs) {
        auto stored = co_await persist(item.blob);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    auto stdout_stored = co_await persist(native->stdout_output);
    auto stderr_stored = co_await persist(native->stderr_output);
    if (!stdout_stored) {
        co_return std::unexpected(stdout_stored.error());
    }
    if (!stderr_stored) {
        co_return std::unexpected(stderr_stored.error());
    }
    co_return std::optional<action_result>{std::move(*native)};
}

runtime::task<void> remote_client::update_action_result(repository& source,
                                                        action_input input,
                                                        digest sandbox,
                                                        const action_result& value) {
    auto source_staged = co_await stage_tree(source, input.inputs, staged_);
    if (!source_staged) {
        co_return std::unexpected(source_staged.error());
    }
    auto toolchain_staged = co_await stage_tree(source, input.toolchain, staged_);
    if (!toolchain_staged) {
        co_return std::unexpected(toolchain_staged.error());
    }
    auto prepared = co_await prepare_action(input, sandbox);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto uploaded = co_await upload_prepared(*prepared);
    if (!uploaded) {
        co_return std::unexpected(uploaded.error());
    }
    auto logical = std::map<std::string, std::vector<std::byte>>{};
    const auto load = [&](digest id) -> runtime::task<void> {
        auto record_bytes = co_await repository_detail::build_access::get(source, id);
        if (!record_bytes) {
            co_return std::unexpected(record_bytes.error());
        }
        auto record = objects::decode_blob(*record_bytes);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        auto bytes = std::vector<std::byte>{};
        bytes.reserve(static_cast<std::size_t>(record->size));
        for (const auto& chunk : record->chunks) {
            auto part = co_await repository_detail::build_access::get(source, chunk);
            if (!part) {
                co_return std::unexpected(part.error());
            }
            bytes.insert(bytes.end(), part->begin(), part->end());
        }
        if (bytes.size() != record->size) {
            co_return std::unexpected(client_error(
                errc::corrupt, "action output blob has the wrong size", to_string(id)));
        }
        logical.insert_or_assign(to_string(id), std::move(bytes));
        co_return result<void>{};
    };
    for (const auto& item : value.outputs) {
        auto loaded = co_await load(item.blob);
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
    }
    auto stdout_loaded = co_await load(value.stdout_output);
    auto stderr_loaded = co_await load(value.stderr_output);
    if (!stdout_loaded) {
        co_return std::unexpected(stdout_loaded.error());
    }
    if (!stderr_loaded) {
        co_return std::unexpected(stderr_loaded.error());
    }
    auto converted = to_reapi_result(
        value,
        selected_,
        BlobResolver{.bytes = [&logical](digest id) -> result<std::vector<std::byte>> {
            const auto found = logical.find(to_string(id));
            if (found == logical.end()) {
                return std::unexpected(
                    client_error(errc::not_found, "action output was not loaded", to_string(id)));
            }
            return found->second;
        }});
    if (!converted) {
        co_return std::unexpected(converted.error());
    }
    for (const auto& [key, bytes] : logical) {
        auto wire = make_digest(selected_, bytes);
        if (!wire) {
            co_return std::unexpected(wire.error());
        }
        auto id = to_liquid_digest(*wire, selected_);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        auto uploaded_blob = co_await write(instance(), *id, bytes);
        if (!uploaded_blob) {
            co_return std::unexpected(uploaded_blob.error());
        }
    }
    auto function = digest_function(selected_);
    if (!function) {
        co_return std::unexpected(function.error());
    }
    auto cache_digest = action_cache_digest(source, input, sandbox, selected_);
    if (!cache_digest) {
        co_return std::unexpected(cache_digest.error());
    }
    auto response = co_await unary<UpdateActionResultRequest, ActionResult>(
        rpc_,
        update_action_result_method,
        UpdateActionResultRequest{.instance_name = instance(),
                                  .action_digest = *cache_digest,
                                  .action_result = std::move(*converted),
                                  .digest_function = *function});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    co_return result<void>{};
}

const remote_config& remote_client::config() const noexcept {
    return rpc_.config();
}

algorithm remote_client::selected_algorithm() const noexcept {
    return selected_;
}

} // namespace liquid::reapi
