#include "liquid/workers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>

#include "build/executor.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "reapi/client.hpp"
#include "reapi/conversion.hpp"
#include "reapi/messages.hpp"
#include "sandbox/environment.hpp"
#include "workers/control.hpp"
#include "workers/protocol.hpp"

namespace liquid {
namespace {

enum class graph_kind : std::uint8_t { bytes, blob, tree };

struct worker_runtime_status {
    std::atomic_bool ready{};
    std::atomic_bool active{};
    std::atomic_uint64_t completed{};
    std::atomic_uint64_t failures{};
};

auto worker_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
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

template <typename Request, typename Response>
runtime::task<Response>
worker_call(const remote_config& config, std::string_view method, const Request& request) {
    auto encoded = workers::encode(request);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto client = reapi::grpc_client{config};
    auto messages = co_await client.call(method, *encoded);
    if (!messages) {
        co_return std::unexpected(messages.error());
    }
    if (messages->size() != 1) {
        co_return std::unexpected(
            worker_error(errc::corrupt, "worker RPC returned the wrong message count"));
    }
    co_return workers::decode<Response>(messages->front());
}

runtime::task<std::vector<std::byte>>
read_remote_object(remote_blob_source& source, const std::string& instance, digest id) {
    auto output = std::vector<std::byte>{};
    auto buffer = std::array<std::byte, 64U * 1024U>{};
    for (;;) {
        auto count = co_await source.read_range(instance, id, output.size(), buffer);
        if (!count) {
            co_return std::unexpected(count.error());
        }
        if (*count > buffer.size()) {
            co_return std::unexpected(worker_error(
                errc::corrupt, "scheduler returned an oversized object", to_string(id)));
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + *count);
        if (*count < buffer.size()) {
            break;
        }
    }
    co_return output;
}

runtime::task<void> hydrate(repository& destination,
                            remote_blob_source& source,
                            const std::string& instance,
                            digest id,
                            graph_kind kind,
                            std::set<std::string>& seen) {
    if (!seen.insert(to_string(id)).second) {
        co_return result<void>{};
    }
    auto bytes = co_await read_remote_object(source, instance, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto stored = co_await repository_detail::build_access::put_record(destination, *bytes, id.alg);
    if (!stored || stored->bytes != id.bytes) {
        co_return std::unexpected(
            stored ? worker_error(errc::corrupt, "scheduler object digest changed", to_string(id))
                   : stored.error());
    }
    if (kind == graph_kind::tree) {
        auto tree = objects::decode_tree(*bytes);
        if (!tree) {
            co_return std::unexpected(tree.error());
        }
        for (const auto& entry : tree->entries) {
            if (entry.kind == node_kind::tree) {
                auto nested = co_await hydrate(
                    destination, source, instance, entry.ref, graph_kind::tree, seen);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
            } else if (entry.kind == node_kind::blob) {
                auto nested = co_await hydrate(
                    destination, source, instance, entry.ref, graph_kind::blob, seen);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
            }
        }
    } else if (kind == graph_kind::blob) {
        auto blob = objects::decode_blob(*bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        for (const auto& chunk : blob->chunks) {
            auto nested =
                co_await hydrate(destination, source, instance, chunk, graph_kind::bytes, seen);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
        }
    }
    co_return result<void>{};
}

runtime::task<void> upload_object(repository& source,
                                  remote_blob_source& destination,
                                  const std::string& instance,
                                  digest id,
                                  graph_kind kind,
                                  std::set<std::string>& seen) {
    if (!seen.insert(to_string(id)).second) {
        co_return result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto uploaded = co_await destination.write(instance, id, *bytes);
    if (!uploaded) {
        co_return std::unexpected(uploaded.error());
    }
    if (kind != graph_kind::blob) {
        co_return result<void>{};
    }
    auto blob = objects::decode_blob(*bytes);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    for (const auto& chunk : blob->chunks) {
        auto nested =
            co_await upload_object(source, destination, instance, chunk, graph_kind::bytes, seen);
        if (!nested) {
            co_return std::unexpected(nested.error());
        }
    }
    co_return result<void>{};
}

runtime::task<std::vector<std::byte>> logical_blob(repository& source, digest id) {
    auto record_bytes = co_await repository_detail::build_access::get(source, id);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    auto record = objects::decode_blob(*record_bytes);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(record->size));
    for (const auto& chunk : record->chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != record->size) {
        co_return std::unexpected(
            worker_error(errc::corrupt, "worker blob has the wrong size", to_string(id)));
    }
    co_return output;
}

auto sbom_document(const attestation& statement) -> result<digest> {
    const auto found = std::ranges::find(
        statement.attributes, std::string{"document"}, &attestation_attribute::name);
    if (found == statement.attributes.end()) {
        return std::unexpected(worker_error(errc::corrupt, "SBOM has no document"));
    }
    const auto* value = std::get_if<std::string>(&found->value);
    if (!value) {
        return std::unexpected(worker_error(errc::corrupt, "SBOM document is not a digest"));
    }
    return parse_digest(*value);
}

runtime::task<std::map<std::string, std::vector<std::byte>>>
upload_result(repository& source,
              remote_blob_source& destination,
              const std::string& instance,
              const action_result& value) {
    auto seen = std::set<std::string>{};
    auto logical = std::map<std::string, std::vector<std::byte>>{};
    auto blob_ids = std::vector<digest>{value.stdout_output, value.stderr_output};
    for (const auto& output : value.outputs) {
        blob_ids.push_back(output.blob);
    }
    for (const auto& id : blob_ids) {
        auto uploaded =
            co_await upload_object(source, destination, instance, id, graph_kind::blob, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
        auto contents = co_await logical_blob(source, id);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        auto content_id = hash(id.alg, *contents);
        if (!content_id) {
            co_return std::unexpected(content_id.error());
        }
        uploaded = co_await destination.write(instance, *content_id, *contents);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
        logical.insert_or_assign(to_string(id), std::move(*contents));
    }
    if (value.provenance) {
        auto uploaded = co_await upload_object(
            source, destination, instance, *value.provenance, graph_kind::bytes, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
    }
    for (const auto& id : value.sboms) {
        auto uploaded =
            co_await upload_object(source, destination, instance, id, graph_kind::bytes, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
        auto bytes = co_await repository_detail::build_access::get(source, id);
        auto statement = bytes ? objects::decode_attestation(*bytes)
                               : result<attestation>{std::unexpected(bytes.error())};
        auto document = statement ? sbom_document(*statement)
                                  : result<digest>{std::unexpected(statement.error())};
        if (!document) {
            co_return std::unexpected(document.error());
        }
        uploaded = co_await upload_object(
            source, destination, instance, *document, graph_kind::bytes, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
    }
    for (const auto& id : value.abis) {
        auto uploaded =
            co_await upload_object(source, destination, instance, id, graph_kind::bytes, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
    }
    for (const auto& id : value.abi_compatibility) {
        auto uploaded =
            co_await upload_object(source, destination, instance, id, graph_kind::bytes, seen);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
    }
    co_return logical;
}

runtime::task<void> heartbeat_loop(remote_config config,
                                   std::string operation,
                                   std::string worker,
                                   std::uint64_t token,
                                   std::chrono::milliseconds interval,
                                   std::shared_ptr<std::atomic_bool> done) {
    while (!done->load()) {
        auto [delay_error] = co_await runtime::delay(interval);
        if (delay_error || done->load()) {
            co_return result<void>{};
        }
        auto response = co_await worker_call<workers::heartbeat_request, workers::acknowledgement>(
            config,
            workers::heartbeat_method,
            {.operation = operation, .worker = worker, .token = token});
        if (!response || !response->accepted) {
            done->store(true);
            co_return response ? result<void>{} : result<void>{std::unexpected(response.error())};
        }
    }
    co_return result<void>{};
}

runtime::task<void> report_failure(const worker_options& options,
                                   const workers::lease_response& lease,
                                   std::string worker,
                                   const error& failure) {
    auto response = co_await worker_call<workers::complete_request, workers::acknowledgement>(
        options.scheduler,
        workers::complete_method,
        {.operation = lease.operation,
         .worker = std::move(worker),
         .token = lease.token,
         .failure = failure.message + (failure.context.empty() ? "" : ": " + failure.context)});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    co_return result<void>{};
}

runtime::task<void> execute_lease(repository& source,
                                  remote& scheduler,
                                  const worker_options& options,
                                  const std::string& worker,
                                  const workers::lease_response& lease) {
    auto action = reapi::decode_message<reapi::Action>(lease.action);
    auto command = reapi::decode_message<reapi::Command>(lease.command);
    auto decoded = action && command
                       ? reapi::from_reapi_action(*action, *command, lease.digest_function)
                       : result<reapi::DecodedAction>{
                             std::unexpected(!action ? action.error() : command.error())};
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    decoded->input.remote_eligible = lease.remote_eligible;
    if (decoded->input.platform.os != host_platform().os ||
        decoded->input.platform.arch != host_platform().arch) {
        co_return std::unexpected(
            worker_error(errc::unsupported,
                         "leased action does not match worker platform",
                         decoded->input.platform.os + "/" + decoded->input.platform.arch));
    }
    auto seen = std::set<std::string>{};
    auto hydrated = co_await hydrate(source,
                                     scheduler.blobs(),
                                     scheduler.config().instance_name,
                                     decoded->input.inputs,
                                     graph_kind::tree,
                                     seen);
    if (!hydrated) {
        co_return std::unexpected(hydrated.error());
    }
    hydrated = co_await hydrate(source,
                                scheduler.blobs(),
                                scheduler.config().instance_name,
                                decoded->input.toolchain,
                                graph_kind::tree,
                                seen);
    if (!hydrated) {
        co_return std::unexpected(hydrated.error());
    }
    auto sandbox = sandbox_detail::definition(decoded->input.platform);
    auto sandbox_bytes = sandbox ? objects::encode(*sandbox)
                                 : result<std::vector<std::byte>>{std::unexpected(sandbox.error())};
    auto sandbox_id = sandbox_bytes ? co_await repository_detail::build_access::put_record(
                                          source, *sandbox_bytes, decoded->sandbox.alg)
                                    : result<digest>{std::unexpected(sandbox_bytes.error())};
    if (!sandbox_id || sandbox_id->bytes != decoded->sandbox.bytes) {
        co_return std::unexpected(
            sandbox_id ? worker_error(errc::unsupported,
                                      "worker sandbox definition does not match action",
                                      to_string(decoded->sandbox))
                       : sandbox_id.error());
    }
    auto action_bytes = objects::encode(objects::action_record{decoded->input, *sandbox_id});
    auto action_id = action_bytes ? co_await repository_detail::build_access::put_record(
                                        source, *action_bytes, decoded->input.inputs.alg)
                                  : result<digest>{std::unexpected(action_bytes.error())};
    if (!action_id) {
        co_return std::unexpected(action_id.error());
    }
    if (action_id->alg != lease.native_action.alg ||
        action_id->bytes != lease.native_action.bytes) {
        co_return std::unexpected(worker_error(
            errc::corrupt, "leased native action digest changed", to_string(lease.native_action)));
    }
    const auto observed_at =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto execution_input = decoded->input;
    for (const auto& name : sandbox->fixed_environment) {
        execution_input.env.erase(name);
    }
    execution_input.remote_eligible = true;
    auto value = co_await build::run_action(
        source, std::move(execution_input), *action_id, *sandbox_id, observed_at);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    value->worker = worker;
    value->schedule = lease.schedule;
    value->record_version = 4;
    auto stored = co_await build_detail::store_result_with_provenance(source, *value);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto signed_result = co_await build::result(source, *stored);
    if (!signed_result) {
        co_return std::unexpected(signed_result.error());
    }
    auto logical = co_await upload_result(
        source, scheduler.blobs(), scheduler.config().instance_name, *signed_result);
    if (!logical) {
        co_return std::unexpected(logical.error());
    }
    auto result_algorithm = reapi::digest_algorithm(lease.digest_function);
    if (!result_algorithm) {
        co_return std::unexpected(result_algorithm.error());
    }
    auto wire = reapi::to_reapi_result(
        *signed_result,
        *result_algorithm,
        reapi::BlobResolver{.bytes = [&logical](digest id) -> result<std::vector<std::byte>> {
            const auto found = logical->find(to_string(id));
            if (found == logical->end()) {
                return std::unexpected(worker_error(
                    errc::not_found, "worker result blob is unavailable", to_string(id)));
            }
            return found->second;
        }});
    auto encoded =
        wire ? reapi::encode(*wire) : result<std::vector<std::byte>>{std::unexpected(wire.error())};
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto accepted = co_await worker_call<workers::complete_request, workers::acknowledgement>(
        options.scheduler,
        workers::complete_method,
        {.operation = lease.operation,
         .worker = worker,
         .token = lease.token,
         .result = std::move(*encoded)});
    if (!accepted) {
        co_return std::unexpected(accepted.error());
    }
    if (!accepted->accepted) {
        co_return std::unexpected(
            worker_error(errc::unavailable, accepted->message, lease.operation));
    }
    co_return result<void>{};
}

} // namespace

runtime::task<void> run_worker(worker_options options) {
    if (options.scheduler.endpoint.empty()) {
        co_return std::unexpected(
            worker_error(errc::invalid_argument, "worker scheduler endpoint is empty"));
    }
    if (options.root_directory.empty()) {
        co_return std::unexpected(
            worker_error(errc::invalid_argument, "worker root directory is empty"));
    }
    options.scheduler.backend = remote_backend::liquid_scheduler;
    auto code = std::error_code{};
    const auto marker = options.root_directory / "LIQUID_REPO";
    auto source = std::filesystem::exists(marker, code)
                      ? co_await repository::open(options.root_directory)
                      : co_await repository::create(options.root_directory);
    if (code) {
        co_return std::unexpected(
            worker_error(errc::io, code.message(), options.root_directory.string()));
    }
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto identity = repository_detail::build_access::signer(*source);
    if (!identity) {
        co_return std::unexpected(identity.error());
    }
    const auto worker = identity->taap_id;
    auto scheduler = co_await remote::connect(options.scheduler);
    if (!scheduler) {
        co_return std::unexpected(scheduler.error());
    }
    auto connection = std::move(*scheduler);
    auto status = std::make_shared<worker_runtime_status>();
    status->ready.store(true);
    runtime::spawn(workers::serve_control(
                       std::move(options.control_listen),
                       workers::control_status{.component = "worker",
                                               .ready = [status] { return status->ready.load(); },
                                               .metrics =
                                                   [status] {
                                                       return std::format(
                                                           "liquid_worker_ready {}\n"
                                                           "liquid_worker_active {}\n"
                                                           "liquid_worker_completed {}\n"
                                                           "liquid_worker_failures {}\n",
                                                           status->ready.load() ? 1 : 0,
                                                           status->active.load() ? 1 : 0,
                                                           status->completed.load(),
                                                           status->failures.load());
                                                   }}),
                   [status](result<void> outcome) {
                       if (!outcome) {
                           status->ready.store(false);
                       }
                   });
    for (;;) {
        auto lease = co_await worker_call<workers::worker_request, workers::lease_response>(
            options.scheduler,
            workers::lease_method,
            {.instance = options.scheduler.instance_name,
             .worker = worker,
             .platform = host_platform(),
             .size_class = options.size_class});
        if (!lease) {
            if (lease.error().code != errc::unavailable && lease.error().code != errc::io) {
                co_return std::unexpected(lease.error());
            }
            status->ready.store(false);
            auto [delay_error] = co_await runtime::delay(options.poll_interval);
            if (delay_error) {
                co_return std::unexpected(
                    worker_error(errc::unavailable, delay_error.message(), worker));
            }
            auto reconnected = co_await remote::connect(options.scheduler);
            if (reconnected) {
                connection = std::move(*reconnected);
                status->ready.store(true);
            }
            continue;
        }
        status->ready.store(true);
        if (!lease->assigned) {
            auto [delay_error] = co_await runtime::delay(options.poll_interval);
            if (delay_error) {
                co_return std::unexpected(
                    worker_error(errc::unavailable, delay_error.message(), worker));
            }
            continue;
        }
        auto done = std::make_shared<std::atomic_bool>();
        status->active.store(true);
        runtime::spawn(heartbeat_loop(options.scheduler,
                                      lease->operation,
                                      worker,
                                      lease->token,
                                      options.poll_interval,
                                      done),
                       [](result<void>) {});
        auto executed = co_await execute_lease(*source, connection, options, worker, *lease);
        done->store(true);
        status->active.store(false);
        if (!executed) {
            ++status->failures;
            auto reported = co_await report_failure(options, *lease, worker, executed.error());
            if (!reported) {
                status->ready.store(false);
                auto [delay_error] = co_await runtime::delay(options.poll_interval);
                if (delay_error) {
                    co_return std::unexpected(
                        worker_error(errc::unavailable, delay_error.message(), worker));
                }
            }
        } else {
            ++status->completed;
        }
    }
}

} // namespace liquid
