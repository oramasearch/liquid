#include "reapi/conversion.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

#include "objects/records.hpp"
#include "protobuf/wire.hpp"
#include "reapi/messages.hpp"

namespace liquid::reapi {
namespace {

constexpr auto directory_mode = std::uint32_t{0040000};
constexpr auto regular_mode = std::uint32_t{0100000};
constexpr auto symlink_mode = std::uint32_t{0120000};
constexpr auto type_mask = std::uint32_t{0170000};
constexpr auto result_metadata_type =
    std::string_view{"type.googleapis.com/liquid.reapi.v1.ActionResultMetadata"};

auto failure(errc code, std::string message, std::string context = "REAPI conversion") -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const Digest& left, const Digest& right) -> bool {
    return left.hash == right.hash && left.size_bytes == right.size_bytes;
}

auto same_digest(const liquid::digest& left, const liquid::digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto mode_properties(std::uint32_t mode) -> NodeProperties {
    return NodeProperties{.unix_mode = UInt32Value{.value = mode}};
}

auto cache_key(const liquid::digest& value, algorithm hash_algorithm, std::uint32_t mode)
    -> std::string {
    return to_string(value) + ":" + std::to_string(static_cast<std::uint8_t>(hash_algorithm)) +
           ":" + std::to_string(mode);
}

auto append_unique(std::vector<DirectoryObject>& output,
                   std::set<std::string>& seen,
                   std::vector<DirectoryObject> values) -> void {
    for (auto& value : values) {
        const auto key = value.digest.hash + ":" + std::to_string(value.digest.size_bytes);
        if (seen.insert(key).second) {
            output.push_back(std::move(value));
        }
    }
}

auto build_directory(liquid::digest root,
                     algorithm hash_algorithm,
                     const TreeSource& source,
                     std::uint32_t mode,
                     std::map<std::string, DirectoryTree>& cache,
                     std::set<std::string>& active) -> result<DirectoryTree>;

auto build_uncached(liquid::digest root,
                    algorithm hash_algorithm,
                    const TreeSource& source,
                    std::uint32_t mode,
                    std::map<std::string, DirectoryTree>& cache,
                    std::set<std::string>& active) -> result<DirectoryTree> {
    auto loaded = source.tree(root);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto entries = loaded->entries;
    std::ranges::sort(entries, {}, &tree_entry::name);
    if (std::ranges::adjacent_find(entries, {}, &tree_entry::name) != entries.end()) {
        return std::unexpected(
            failure(errc::invalid_argument, "tree contains duplicate entry names"));
    }

    auto directory = Directory{.node_properties = mode_properties(mode)};
    auto objects = std::vector<DirectoryObject>{};
    auto seen_objects = std::set<std::string>{};
    for (const auto& entry : entries) {
        const auto kind = entry.mode & type_mask;
        if (kind == symlink_mode) {
            auto contents = source.blob(entry.ref);
            if (!contents) {
                return std::unexpected(contents.error());
            }
            directory.symlinks.push_back(
                {.name = entry.name,
                 .target =
                     std::string{reinterpret_cast<const char*>(contents->data()), contents->size()},
                 .node_properties = mode_properties(entry.mode)});
            continue;
        }
        if (entry.kind == node_kind::tree || entry.kind == node_kind::repo ||
            kind == directory_mode) {
            auto child =
                build_directory(entry.ref, hash_algorithm, source, entry.mode, cache, active);
            if (!child) {
                return std::unexpected(child.error());
            }
            directory.directories.push_back({.name = entry.name, .digest = child->root_digest});
            append_unique(objects, seen_objects, std::move(child->objects));
            continue;
        }
        if (entry.kind != node_kind::blob || kind != regular_mode) {
            return std::unexpected(
                failure(errc::unsupported, "tree entry cannot be represented by REAPI"));
        }
        auto contents = source.blob(entry.ref);
        if (!contents) {
            return std::unexpected(contents.error());
        }
        auto content_digest = make_digest(hash_algorithm, *contents);
        if (!content_digest) {
            return std::unexpected(content_digest.error());
        }
        directory.files.push_back({.name = entry.name,
                                   .digest = *content_digest,
                                   .is_executable = (entry.mode & 0111U) != 0,
                                   .node_properties = mode_properties(entry.mode)});
    }

    auto bytes = encode(directory);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto encoded_digest = make_digest(hash_algorithm, *bytes);
    if (!encoded_digest) {
        return std::unexpected(encoded_digest.error());
    }
    objects.push_back({.digest = *encoded_digest, .directory = directory, .bytes = *bytes});
    return DirectoryTree{.root_digest = *encoded_digest, .objects = std::move(objects)};
}

auto build_directory(liquid::digest root,
                     algorithm hash_algorithm,
                     const TreeSource& source,
                     std::uint32_t mode,
                     std::map<std::string, DirectoryTree>& cache,
                     std::set<std::string>& active) -> result<DirectoryTree> {
    const auto key = cache_key(root, hash_algorithm, mode);
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    const auto inserted = active.insert(key);
    if (!inserted.second) {
        return std::unexpected(failure(errc::corrupt, "tree contains a cycle"));
    }
    auto converted = build_uncached(root, hash_algorithm, source, mode, cache, active);
    active.erase(inserted.first);
    if (converted) {
        cache.insert_or_assign(key, *converted);
    }
    return converted;
}

auto property(const Platform& platform, std::string_view name) -> std::optional<std::string> {
    for (const auto& item : platform.properties) {
        if (item.name == name) {
            return item.value;
        }
    }
    return std::nullopt;
}

auto encoded_property(liquid::digest value) -> std::string {
    return to_string(value);
}

auto action_platform(const action_input& input, liquid::digest sandbox) -> Platform {
    auto output = Platform{
        .properties = {{.name = "ISA", .value = input.platform.arch},
                       {.name = "OSFamily", .value = input.platform.os},
                       {.name = "liquid.inputs", .value = encoded_property(input.inputs)},
                       {.name = "liquid.sandbox", .value = encoded_property(sandbox)},
                       {.name = "liquid.toolchain", .value = encoded_property(input.toolchain)}}};
    std::ranges::sort(output.properties, [](const auto& left, const auto& right) {
        return std::pair{left.name, left.value} < std::pair{right.name, right.value};
    });
    return output;
}

auto parse_required_property(const Platform& platform, std::string_view name)
    -> result<liquid::digest> {
    auto text = property(platform, name);
    if (!text) {
        return std::unexpected(
            failure(errc::corrupt, "missing platform property " + std::string{name}));
    }
    return parse_digest(*text);
}

auto has_function(std::span<const DigestFunctionValue> values, DigestFunctionValue expected)
    -> bool {
    return std::ranges::find(values, expected) != values.end();
}

auto timestamp(std::chrono::sys_seconds value) -> Timestamp {
    return {.seconds = value.time_since_epoch().count()};
}

auto validate_timing(const action_timing& value) -> result<void> {
    if (value.queued.count() < 0 || value.sandbox.count() < 0 || value.exec.count() < 0 ||
        value.hash.count() < 0 || value.store.count() < 0) {
        return std::unexpected(failure(errc::invalid_argument, "action timing is negative"));
    }
    return {};
}

auto result_metadata(const action_result& value) -> result<Any> {
    auto record = objects::encode(value);
    if (!record) {
        return std::unexpected(record.error());
    }
    auto output = protobuf::writer{};
    output.bytes(1, *record);
    return Any{.type_url = std::string{result_metadata_type}, .value = std::move(output).take()};
}

auto decode_result_metadata(const Any& value) -> result<action_result> {
    auto input = protobuf::reader{value.value};
    auto record = std::optional<std::vector<std::byte>>{};
    while (!input.finished()) {
        auto field = input.next();
        if (!field) {
            return std::unexpected(field.error());
        }
        if (!*field) {
            break;
        }
        if ((*field)->number != 1) {
            continue;
        }
        auto bytes = protobuf::require_bytes(**field, "ActionResultMetadata.record");
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        record = std::vector<std::byte>{bytes->begin(), bytes->end()};
    }
    if (!record) {
        return std::unexpected(failure(errc::corrupt, "result metadata is missing its record"));
    }
    auto decoded = objects::decode_result(*record);
    if (!decoded) {
        return std::unexpected(failure(decoded.error().code,
                                       decoded.error().message + " in metadata record of " +
                                           std::to_string(record->size()) + " bytes"));
    }
    return decoded;
}

auto resolve_blob(const BlobResolver& blobs, liquid::digest value)
    -> result<std::vector<std::byte>> {
    if (!blobs.bytes) {
        return std::unexpected(failure(errc::invalid_argument, "blob resolver is missing"));
    }
    return blobs.bytes(value);
}

auto output_digest(const std::optional<Digest>& value,
                   std::span<const std::byte> inline_bytes,
                   algorithm hash_algorithm) -> result<liquid::digest> {
    if (value) {
        return to_liquid_digest(*value, hash_algorithm);
    }
    auto digest = make_digest(hash_algorithm, inline_bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return to_liquid_digest(*digest, hash_algorithm);
}

auto unix_mode(const std::optional<NodeProperties>& properties, std::uint32_t fallback)
    -> std::uint32_t {
    if (properties && properties->unix_mode) {
        return properties->unix_mode->value;
    }
    return fallback;
}

} // namespace

auto DirectoryCache::convert(liquid::digest root,
                             algorithm hash_algorithm,
                             const TreeSource& source,
                             std::uint32_t root_mode) -> result<DirectoryTree> {
    if (!source.tree || !source.blob) {
        return std::unexpected(failure(errc::invalid_argument, "tree source is incomplete"));
    }
    auto active = std::set<std::string>{};
    auto converted = build_directory(root, hash_algorithm, source, root_mode, entries_, active);
    if (!converted) {
        return std::unexpected(converted.error());
    }
    return converted;
}

auto DirectoryCache::size() const noexcept -> std::size_t {
    return entries_.size();
}

auto select_digest_algorithm(const ServerCapabilities& capabilities, bool execution)
    -> result<algorithm> {
    auto values = std::vector<DigestFunctionValue>{};
    if (execution) {
        if (!capabilities.execution_capabilities) {
            return std::unexpected(
                failure(errc::unsupported, "backend does not advertise execution capabilities"));
        }
        values = capabilities.execution_capabilities->digest_functions;
        if (values.empty() &&
            capabilities.execution_capabilities->digest_function != DigestFunctionValue::unknown) {
            values.push_back(capabilities.execution_capabilities->digest_function);
        }
    } else {
        if (!capabilities.cache_capabilities) {
            return std::unexpected(
                failure(errc::unsupported, "backend does not advertise cache capabilities"));
        }
        values = capabilities.cache_capabilities->digest_functions;
    }
    if (has_function(values, DigestFunctionValue::blake3)) {
        return algorithm::blake3;
    }
    if (has_function(values, DigestFunctionValue::sha256)) {
        return algorithm::sha256;
    }
    return std::unexpected(
        failure(errc::unsupported, "backend supports neither BLAKE3 nor SHA-256"));
}

auto digest_function(algorithm hash_algorithm) -> result<DigestFunctionValue> {
    if (hash_algorithm == algorithm::blake3) {
        return DigestFunctionValue::blake3;
    }
    if (hash_algorithm == algorithm::sha256) {
        return DigestFunctionValue::sha256;
    }
    return std::unexpected(failure(errc::unsupported, "digest is not supported by REAPI"));
}

auto digest_algorithm(DigestFunctionValue function) -> result<algorithm> {
    if (function == DigestFunctionValue::blake3) {
        return algorithm::blake3;
    }
    if (function == DigestFunctionValue::sha256) {
        return algorithm::sha256;
    }
    return std::unexpected(failure(errc::unsupported, "REAPI digest function is unsupported"));
}

auto make_digest(algorithm hash_algorithm, std::span<const std::byte> bytes) -> result<Digest> {
    auto value = hash(hash_algorithm, bytes);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto text = to_string(*value);
    const auto separator = text.find(':');
    return Digest{.hash = text.substr(separator + 1),
                  .size_bytes = static_cast<std::int64_t>(bytes.size())};
}

auto to_liquid_digest(const Digest& value, algorithm hash_algorithm) -> result<liquid::digest> {
    if (value.hash.size() != 64 || value.size_bytes < 0) {
        return std::unexpected(failure(errc::corrupt, "invalid REAPI digest"));
    }
    const auto prefix = hash_algorithm == algorithm::blake3   ? std::string{"blake3:"}
                        : hash_algorithm == algorithm::sha256 ? std::string{"sha256:"}
                                                              : std::string{};
    if (prefix.empty()) {
        return std::unexpected(failure(errc::unsupported, "REAPI digest algorithm is unsupported"));
    }
    return parse_digest(prefix + value.hash);
}

auto to_reapi_action(const action_input& input,
                     const Digest& input_root_digest,
                     liquid::digest sandbox,
                     algorithm hash_algorithm) -> result<ActionMessages> {
    auto platform = action_platform(input, sandbox);
    auto command = Command{.arguments = input.command,
                           .platform = platform,
                           .working_directory = input.cwd,
                           .output_paths = input.outputs};
    for (const auto& [name, value] : input.env) {
        command.environment_variables.push_back({.name = name, .value = value});
    }
    std::ranges::sort(command.output_paths);
    command.output_paths.erase(std::ranges::unique(command.output_paths).begin(),
                               command.output_paths.end());
    auto command_bytes = encode(command);
    if (!command_bytes) {
        return std::unexpected(command_bytes.error());
    }
    auto command_digest = make_digest(hash_algorithm, *command_bytes);
    if (!command_digest) {
        return std::unexpected(command_digest.error());
    }
    auto action = Action{.command_digest = *command_digest,
                         .input_root_digest = input_root_digest,
                         .platform = platform};
    if (input.timeout) {
        action.timeout = Duration{.seconds = input.timeout->count()};
    }
    auto action_bytes = encode(action);
    if (!action_bytes) {
        return std::unexpected(action_bytes.error());
    }
    auto action_digest = make_digest(hash_algorithm, *action_bytes);
    if (!action_digest) {
        return std::unexpected(action_digest.error());
    }
    return ActionMessages{.command = std::move(command),
                          .command_digest = *command_digest,
                          .action = std::move(action),
                          .action_digest = *action_digest};
}

auto from_reapi_action(const Action& action, const Command& command, DigestFunctionValue function)
    -> result<DecodedAction> {
    auto hash_algorithm = digest_algorithm(function);
    if (!hash_algorithm) {
        return std::unexpected(hash_algorithm.error());
    }
    if (!action.command_digest || !action.input_root_digest) {
        return std::unexpected(failure(errc::corrupt, "REAPI Action is missing a digest"));
    }
    auto command_bytes = encode(command);
    if (!command_bytes) {
        return std::unexpected(command_bytes.error());
    }
    auto actual_command = make_digest(*hash_algorithm, *command_bytes);
    if (!actual_command || !same_digest(*actual_command, *action.command_digest)) {
        return std::unexpected(failure(errc::corrupt, "REAPI Command digest does not match"));
    }
    const auto* platform = action.platform    ? &*action.platform
                           : command.platform ? &*command.platform
                                              : nullptr;
    if (!platform) {
        return std::unexpected(failure(errc::corrupt, "REAPI platform is missing"));
    }
    auto inputs = parse_required_property(*platform, "liquid.inputs");
    auto toolchain = parse_required_property(*platform, "liquid.toolchain");
    auto sandbox = parse_required_property(*platform, "liquid.sandbox");
    auto os = property(*platform, "OSFamily");
    auto arch = property(*platform, "ISA");
    if (!inputs || !toolchain || !sandbox || !os || !arch) {
        return std::unexpected(failure(errc::corrupt, "REAPI platform is incomplete"));
    }
    auto environment = std::map<std::string, std::string>{};
    for (const auto& item : command.environment_variables) {
        environment.insert_or_assign(item.name, item.value);
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
            return std::unexpected(
                failure(errc::corrupt, "REAPI Action timeout is not a whole non-negative second"));
        }
        timeout = std::chrono::seconds{action.timeout->seconds};
    }
    return DecodedAction{.input = {.command = command.arguments,
                                   .cwd = command.working_directory,
                                   .env = std::move(environment),
                                   .outputs = std::move(outputs),
                                   .inputs = *inputs,
                                   .toolchain = *toolchain,
                                   .platform = {.os = *os, .arch = *arch},
                                   .timeout = timeout,
                                   .remote_eligible = true},
                         .sandbox = *sandbox,
                         .input_root_digest = *action.input_root_digest};
}

auto to_reapi_result(const action_result& value,
                     algorithm hash_algorithm,
                     const BlobResolver& blobs) -> result<ActionResult> {
    if (value.exit < std::numeric_limits<std::int32_t>::min() ||
        value.exit > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(failure(errc::invalid_argument, "exit code is outside int32"));
    }
    auto timing = validate_timing(value.timing);
    if (!timing) {
        return std::unexpected(timing.error());
    }

    auto output = ActionResult{.exit_code = static_cast<std::int32_t>(value.exit)};
    for (const auto& item : value.outputs) {
        auto contents = resolve_blob(blobs, item.blob);
        if (!contents) {
            return std::unexpected(contents.error());
        }
        auto digest = make_digest(hash_algorithm, *contents);
        if (!digest) {
            return std::unexpected(digest.error());
        }
        const auto kind = item.mode & type_mask;
        if (kind == 0 || kind == regular_mode) {
            const auto complete_mode = kind == 0 ? regular_mode | item.mode : item.mode;
            output.output_files.push_back({.path = item.path,
                                           .digest = *digest,
                                           .is_executable = (item.mode & 0111U) != 0,
                                           .node_properties = mode_properties(complete_mode)});
            continue;
        }
        if (kind == directory_mode) {
            output.output_directories.push_back(
                {.path = item.path, .root_directory_digest = *digest});
            continue;
        }
        if (kind == symlink_mode) {
            output.output_symlinks.push_back(
                {.path = item.path,
                 .target =
                     std::string{reinterpret_cast<const char*>(contents->data()), contents->size()},
                 .node_properties = mode_properties(item.mode)});
            continue;
        }
        return std::unexpected(
            failure(errc::unsupported, "action output mode cannot be represented by REAPI"));
    }

    auto stdout_bytes = resolve_blob(blobs, value.stdout_output);
    auto stderr_bytes = resolve_blob(blobs, value.stderr_output);
    if (!stdout_bytes) {
        return std::unexpected(stdout_bytes.error());
    }
    if (!stderr_bytes) {
        return std::unexpected(stderr_bytes.error());
    }
    auto stdout_digest = make_digest(hash_algorithm, *stdout_bytes);
    auto stderr_digest = make_digest(hash_algorithm, *stderr_bytes);
    if (!stdout_digest) {
        return std::unexpected(stdout_digest.error());
    }
    if (!stderr_digest) {
        return std::unexpected(stderr_digest.error());
    }
    output.stdout_digest = *stdout_digest;
    output.stderr_digest = *stderr_digest;

    auto metadata = ExecutedActionMetadata{.worker = value.worker};
    if (value.started_at) {
        metadata.queued_timestamp = timestamp(std::chrono::time_point_cast<std::chrono::seconds>(
            *value.started_at - value.timing.queued));
        metadata.worker_start_timestamp = timestamp(*value.started_at);
        metadata.execution_start_timestamp = timestamp(*value.started_at);
    }
    if (value.finished_at) {
        metadata.worker_completed_timestamp = timestamp(*value.finished_at);
        metadata.execution_completed_timestamp = timestamp(*value.finished_at);
    }
    metadata.virtual_execution_duration = Duration{
        .seconds = std::chrono::duration_cast<std::chrono::seconds>(value.timing.exec).count(),
        .nanos = static_cast<std::int32_t>((value.timing.exec % std::chrono::seconds{1}).count() *
                                           1'000'000)};
    auto auxiliary = result_metadata(value);
    if (!auxiliary) {
        return std::unexpected(auxiliary.error());
    }
    metadata.auxiliary_metadata.push_back(std::move(*auxiliary));
    output.execution_metadata = std::move(metadata);
    return output;
}

auto from_reapi_result(const ActionResult& value,
                       liquid::digest action,
                       algorithm hash_algorithm,
                       bool validate_action) -> result<action_result> {
    if (value.execution_metadata) {
        for (const auto& metadata : value.execution_metadata->auxiliary_metadata) {
            if (metadata.type_url != result_metadata_type) {
                continue;
            }
            auto decoded = decode_result_metadata(metadata);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (validate_action && !same_digest(decoded->action, action)) {
                return std::unexpected(
                    failure(errc::corrupt, "result metadata belongs to another action"));
            }
            return decoded;
        }
    }

    auto empty = std::span<const std::byte>{};
    auto stdout_output = output_digest(value.stdout_digest, value.stdout_raw, hash_algorithm);
    auto stderr_output = output_digest(value.stderr_digest, value.stderr_raw, hash_algorithm);
    if (!stdout_output) {
        return std::unexpected(stdout_output.error());
    }
    if (!stderr_output) {
        return std::unexpected(stderr_output.error());
    }
    auto output = action_result{.action = action,
                                .exit = value.exit_code,
                                .stdout_output = *stdout_output,
                                .stderr_output = *stderr_output,
                                .record_version = 3};
    if (value.execution_metadata) {
        output.worker = value.execution_metadata->worker;
        if (value.execution_metadata->execution_start_timestamp) {
            output.started_at = std::chrono::sys_seconds{
                std::chrono::seconds{value.execution_metadata->execution_start_timestamp->seconds}};
        }
        if (value.execution_metadata->execution_completed_timestamp) {
            output.finished_at = std::chrono::sys_seconds{std::chrono::seconds{
                value.execution_metadata->execution_completed_timestamp->seconds}};
        }
        if (value.execution_metadata->virtual_execution_duration) {
            const auto& duration = *value.execution_metadata->virtual_execution_duration;
            output.timing.exec = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds{duration.seconds} + std::chrono::nanoseconds{duration.nanos});
        }
    }
    output.observed_at = output.finished_at.value_or(std::chrono::sys_seconds{});

    for (const auto& item : value.output_files) {
        auto blob = output_digest(item.digest, item.contents, hash_algorithm);
        if (!blob) {
            return std::unexpected(blob.error());
        }
        const auto fallback = regular_mode | (item.is_executable ? 0755U : 0644U);
        output.outputs.push_back({.path = item.path,
                                  .mode = unix_mode(item.node_properties, fallback) & 07777U,
                                  .blob = *blob});
    }
    for (const auto& item : value.output_directories) {
        const auto& digest =
            item.root_directory_digest ? item.root_directory_digest : item.tree_digest;
        if (!digest) {
            return std::unexpected(
                failure(errc::corrupt, "REAPI output directory is missing its digest"));
        }
        auto blob = output_digest(digest, empty, hash_algorithm);
        if (!blob) {
            return std::unexpected(blob.error());
        }
        output.outputs.push_back(
            {.path = item.path, .mode = directory_mode | 0755U, .blob = *blob});
    }
    const auto add_symlinks = [&](const std::vector<OutputSymlink>& values) -> result<void> {
        for (const auto& item : values) {
            const auto bytes = std::as_bytes(std::span{item.target});
            auto blob = output_digest(std::nullopt, bytes, hash_algorithm);
            if (!blob) {
                return std::unexpected(blob.error());
            }
            output.outputs.push_back({.path = item.path,
                                      .mode = unix_mode(item.node_properties, symlink_mode | 0777U),
                                      .blob = *blob});
        }
        return {};
    };
    auto legacy_files = add_symlinks(value.output_file_symlinks);
    auto legacy_directories = add_symlinks(value.output_directory_symlinks);
    auto symlinks = add_symlinks(value.output_symlinks);
    if (!legacy_files) {
        return std::unexpected(legacy_files.error());
    }
    if (!legacy_directories) {
        return std::unexpected(legacy_directories.error());
    }
    if (!symlinks) {
        return std::unexpected(symlinks.error());
    }
    std::ranges::sort(output.outputs, {}, &action_output::path);
    return output;
}

} // namespace liquid::reapi
