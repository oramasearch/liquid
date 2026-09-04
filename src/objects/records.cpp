#include "objects/records.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/attestation.hpp"

namespace liquid::objects {
namespace {

namespace cbor = encoding::cbor;

auto corrupt(std::string message) -> error {
    return {errc::corrupt, std::move(message), "object record"};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto byte_string(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

auto encode_digest(const digest& value) -> cbor::value {
    return map({{"alg", cbor::value{static_cast<std::uint64_t>(value.alg)}},
                {"bytes", byte_string(value.bytes)}});
}

auto decode_digest(const cbor::value& value) -> result<digest> {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields)
        return std::unexpected(corrupt("digest is not a map"));
    const cbor::value* alg = nullptr;
    const cbor::value* bytes = nullptr;
    for (const auto& [key, item] : *fields) {
        const auto* name = std::get_if<std::string>(&key.data);
        if (!name)
            continue;
        if (*name == "alg")
            alg = &item;
        if (*name == "bytes")
            bytes = &item;
    }
    const auto* algorithm = alg ? std::get_if<std::uint64_t>(&alg->data) : nullptr;
    const auto* raw = bytes ? std::get_if<cbor::value::bytes>(&bytes->data) : nullptr;
    if (!algorithm || !raw || raw->size() != digest{}.bytes.size() || *algorithm < 1 ||
        *algorithm > 3)
        return std::unexpected(corrupt("invalid digest"));
    digest output{static_cast<liquid::algorithm>(*algorithm), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto encode_node_id(const node_id& value) -> cbor::value {
    return byte_string(value);
}

auto decode_node_id(const cbor::value& value) -> result<node_id> {
    const auto* raw = std::get_if<cbor::value::bytes>(&value.data);
    if (!raw || raw->size() != node_id{}.size())
        return std::unexpected(corrupt("invalid node identifier"));
    node_id output{};
    std::ranges::copy(*raw, output.begin());
    return output;
}

auto find(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&source.data);
    if (!fields)
        return nullptr;
    for (const auto& [key, value] : *fields) {
        if (const auto* text = std::get_if<std::string>(&key.data); text && *text == name)
            return &value;
    }
    return nullptr;
}

auto record(const cbor::value& source, std::string_view expected) -> result<void> {
    const auto* version = find(source, "v");
    const auto* kind = find(source, "kind");
    const auto* v = version ? std::get_if<std::uint64_t>(&version->data) : nullptr;
    const auto* k = kind ? std::get_if<std::string>(&kind->data) : nullptr;
    if (!v || *v != 1 || !k || *k != expected)
        return std::unexpected(corrupt("unsupported record kind or version"));
    return {};
}

auto decode(std::span<const std::byte> bytes, std::string_view kind) -> result<cbor::value> {
    auto decoded = cbor::decode(bytes);
    if (!decoded)
        return std::unexpected(decoded.error());
    auto checked = record(*decoded, kind);
    if (!checked)
        return std::unexpected(checked.error());
    return decoded;
}

auto encode_change_id(const change_id& value) -> cbor::value {
    return encode_digest(value.value);
}

auto decode_change_id(const cbor::value& value) -> result<change_id> {
    auto decoded = decode_digest(value);
    if (!decoded)
        return std::unexpected(decoded.error());
    return change_id{*decoded};
}

auto encode_actor(const actor& value) -> cbor::value {
    return map({{"taap_id", cbor::value{value.taap_id}},
                {"algorithm", cbor::value{value.algorithm}},
                {"fingerprint", cbor::value{value.fingerprint}},
                {"public_key", byte_string(value.public_key)},
                {"key_fingerprint", cbor::value{value.key_fingerprint}}});
}

auto decode_actor(const cbor::value& value) -> result<actor> {
    const auto* taap_id = find(value, "taap_id");
    const auto* algorithm = find(value, "algorithm");
    const auto* fingerprint = find(value, "fingerprint");
    const auto* public_key = find(value, "public_key");
    const auto* key_fingerprint = find(value, "key_fingerprint");
    const auto* identifier = taap_id ? std::get_if<std::string>(&taap_id->data) : nullptr;
    const auto* algorithm_text = algorithm ? std::get_if<std::string>(&algorithm->data) : nullptr;
    const auto* fingerprint_text =
        fingerprint ? std::get_if<std::string>(&fingerprint->data) : nullptr;
    const auto* key = public_key ? std::get_if<cbor::value::bytes>(&public_key->data) : nullptr;
    const auto* key_fingerprint_text =
        key_fingerprint ? std::get_if<std::string>(&key_fingerprint->data) : nullptr;
    if (!identifier || !algorithm_text || !fingerprint_text || !key || !key_fingerprint_text) {
        return std::unexpected(corrupt("invalid attestation issuer"));
    }
    return actor{*identifier, *algorithm_text, *fingerprint_text, *key, *key_fingerprint_text};
}

auto encode_attribute_value(const attestation_value& value) -> cbor::value {
    return std::visit(
        [](const auto& item) -> cbor::value {
            using value_type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<value_type, std::vector<std::string>>) {
                auto output = cbor::value::array{};
                for (const auto& entry : item) {
                    output.emplace_back(entry);
                }
                return cbor::value{std::move(output)};
            } else {
                return cbor::value{item};
            }
        },
        value);
}

auto decode_text_array(const cbor::value* value, std::string_view field)
    -> result<std::vector<std::string>>;

auto encode_text_map(const std::map<std::string, std::string>& input) -> cbor::value {
    auto output = cbor::value::map{};
    for (const auto& [name, value] : input) {
        output.emplace_back(name, value);
    }
    return cbor::value{std::move(output)};
}

auto encode_resource_descriptor(const slsa_resource_descriptor& value) -> cbor::value {
    auto fields = cbor::value::map{{"uri", cbor::value{value.uri}},
                                   {"digest", encode_text_map(value.digest)}};
    if (value.name) {
        fields.emplace_back("name", *value.name);
    }
    return cbor::value{std::move(fields)};
}

auto encode_resource_descriptors(std::span<const slsa_resource_descriptor> values) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.push_back(encode_resource_descriptor(value));
    }
    return cbor::value{std::move(output)};
}

auto encode_slsa_provenance(const slsa_provenance_v1& value) -> cbor::value {
    auto command = cbor::value::array{};
    for (const auto& argument : value.build_definition.external_parameters.command) {
        command.emplace_back(argument);
    }
    const auto external_parameters = map(
        {{"command", cbor::value{std::move(command)}},
         {"cwd", cbor::value{value.build_definition.external_parameters.cwd}},
         {"environment", encode_text_map(value.build_definition.external_parameters.environment)}});
    const auto internal_parameters =
        map({{"toolchain",
              encode_resource_descriptor(value.build_definition.internal_parameters.toolchain)},
             {"sandbox",
              encode_resource_descriptor(value.build_definition.internal_parameters.sandbox)},
             {"platform", encode_text_map(value.build_definition.internal_parameters.platform)}});
    const auto build_definition =
        map({{"buildType", cbor::value{value.build_definition.build_type}},
             {"externalParameters", external_parameters},
             {"internalParameters", internal_parameters},
             {"resolvedDependencies",
              encode_resource_descriptors(value.build_definition.resolved_dependencies)}});
    const auto builder = map({{"id", cbor::value{value.run_details.builder.id}}});
    const auto metadata =
        map({{"invocationId", cbor::value{value.run_details.metadata.invocation_id}},
             {"startedOn", cbor::value{value.run_details.metadata.started_on}},
             {"finishedOn", cbor::value{value.run_details.metadata.finished_on}}});
    const auto run_details =
        map({{"builder", builder},
             {"metadata", metadata},
             {"byproducts", encode_resource_descriptors(value.run_details.byproducts)}});
    return map({{"buildDefinition", build_definition}, {"runDetails", run_details}});
}

auto decode_attribute_value(const cbor::value& value) -> result<attestation_value> {
    if (const auto* text = std::get_if<std::string>(&value.data)) {
        return *text;
    }
    if (const auto* integer = std::get_if<std::uint64_t>(&value.data)) {
        return *integer;
    }
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        return *boolean;
    }
    const auto* list = std::get_if<cbor::value::array>(&value.data);
    if (!list) {
        return std::unexpected(corrupt("invalid attestation attribute value"));
    }
    auto output = std::vector<std::string>{};
    for (const auto& entry : *list) {
        const auto* text = std::get_if<std::string>(&entry.data);
        if (!text) {
            return std::unexpected(corrupt("invalid attestation attribute list"));
        }
        output.push_back(*text);
    }
    return attestation_value{std::move(output)};
}

auto decode_text_map(const cbor::value* value, std::string_view description)
    -> result<std::map<std::string, std::string>> {
    const auto* fields = value ? std::get_if<cbor::value::map>(&value->data) : nullptr;
    if (!fields) {
        return std::unexpected(corrupt(std::string{description} + " is not a map"));
    }
    auto output = std::map<std::string, std::string>{};
    for (const auto& [key_value, item_value] : *fields) {
        const auto* key = std::get_if<std::string>(&key_value.data);
        const auto* item = std::get_if<std::string>(&item_value.data);
        if (!key || !item || !output.emplace(*key, *item).second) {
            return std::unexpected(corrupt("invalid " + std::string{description}));
        }
    }
    return output;
}

auto decode_resource_descriptor(const cbor::value& value) -> result<slsa_resource_descriptor> {
    const auto* uri_value = find(value, "uri");
    const auto* digest_value = find(value, "digest");
    const auto* uri = uri_value ? std::get_if<std::string>(&uri_value->data) : nullptr;
    auto digests = decode_text_map(digest_value, "resource digest");
    if (!uri || !digests) {
        return std::unexpected(corrupt("invalid SLSA resource descriptor"));
    }
    auto output = slsa_resource_descriptor{*uri, std::move(*digests), std::nullopt};
    if (const auto* name_value = find(value, "name")) {
        const auto* name = std::get_if<std::string>(&name_value->data);
        if (!name) {
            return std::unexpected(corrupt("invalid SLSA resource name"));
        }
        output.name = *name;
    }
    return output;
}

auto decode_resource_descriptors(const cbor::value* value, std::string_view description)
    -> result<std::vector<slsa_resource_descriptor>> {
    const auto* entries = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!entries) {
        return std::unexpected(corrupt(std::string{description} + " is not an array"));
    }
    auto output = std::vector<slsa_resource_descriptor>{};
    output.reserve(entries->size());
    for (const auto& entry : *entries) {
        auto decoded = decode_resource_descriptor(entry);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.push_back(std::move(*decoded));
    }
    return output;
}

auto decode_slsa_provenance(const cbor::value& value) -> result<slsa_provenance_v1> {
    const auto* build_definition = find(value, "buildDefinition");
    const auto* run_details = find(value, "runDetails");
    if (!build_definition || !run_details) {
        return std::unexpected(corrupt("invalid SLSA provenance predicate"));
    }
    const auto* build_type_value = find(*build_definition, "buildType");
    const auto* external = find(*build_definition, "externalParameters");
    const auto* internal = find(*build_definition, "internalParameters");
    const auto* build_type =
        build_type_value ? std::get_if<std::string>(&build_type_value->data) : nullptr;
    if (!build_type || !external || !internal) {
        return std::unexpected(corrupt("invalid SLSA build definition"));
    }
    auto command = decode_text_array(find(*external, "command"), "SLSA command");
    const auto* cwd_value = find(*external, "cwd");
    const auto* cwd = cwd_value ? std::get_if<std::string>(&cwd_value->data) : nullptr;
    auto environment = decode_text_map(find(*external, "environment"), "SLSA environment");
    auto toolchain = find(*internal, "toolchain")
                         ? decode_resource_descriptor(*find(*internal, "toolchain"))
                         : result<slsa_resource_descriptor>{
                               std::unexpected(corrupt("SLSA toolchain is missing"))};
    auto sandbox =
        find(*internal, "sandbox")
            ? decode_resource_descriptor(*find(*internal, "sandbox"))
            : result<slsa_resource_descriptor>{std::unexpected(corrupt("SLSA sandbox is missing"))};
    auto platform = decode_text_map(find(*internal, "platform"), "SLSA platform");
    auto dependencies = decode_resource_descriptors(find(*build_definition, "resolvedDependencies"),
                                                    "SLSA resolved dependencies");
    if (!command || !cwd || !environment || !toolchain || !sandbox || !platform || !dependencies) {
        return std::unexpected(corrupt("invalid SLSA build parameters"));
    }
    const auto* builder_value = find(*run_details, "builder");
    const auto* metadata_value = find(*run_details, "metadata");
    const auto* builder_id_value = builder_value ? find(*builder_value, "id") : nullptr;
    const auto* invocation_value = metadata_value ? find(*metadata_value, "invocationId") : nullptr;
    const auto* started_value = metadata_value ? find(*metadata_value, "startedOn") : nullptr;
    const auto* finished_value = metadata_value ? find(*metadata_value, "finishedOn") : nullptr;
    const auto* builder_id =
        builder_id_value ? std::get_if<std::string>(&builder_id_value->data) : nullptr;
    const auto* invocation =
        invocation_value ? std::get_if<std::string>(&invocation_value->data) : nullptr;
    const auto* started = started_value ? std::get_if<std::string>(&started_value->data) : nullptr;
    const auto* finished =
        finished_value ? std::get_if<std::string>(&finished_value->data) : nullptr;
    auto byproducts =
        decode_resource_descriptors(find(*run_details, "byproducts"), "SLSA byproducts");
    if (!builder_id || !invocation || !started || !finished || !byproducts) {
        return std::unexpected(corrupt("invalid SLSA run details"));
    }
    return slsa_provenance_v1{
        .build_definition = {.build_type = *build_type,
                             .external_parameters = {std::move(*command),
                                                     *cwd,
                                                     std::move(*environment)},
                             .internal_parameters = {std::move(*toolchain),
                                                     std::move(*sandbox),
                                                     std::move(*platform)},
                             .resolved_dependencies = std::move(*dependencies)},
        .run_details = {{*builder_id}, {*invocation, *started, *finished}, std::move(*byproducts)}};
}

auto parse_subject_kind(std::string_view value) -> result<attestation_subject_kind> {
    if (value == "actor") {
        return attestation_subject_kind::actor;
    }
    if (value == "drip") {
        return attestation_subject_kind::drip;
    }
    if (value == "change") {
        return attestation_subject_kind::change;
    }
    if (value == "artifact") {
        return attestation_subject_kind::artifact;
    }
    if (value == "model") {
        return attestation_subject_kind::model;
    }
    if (value == "library") {
        return attestation_subject_kind::library;
    }
    return std::unexpected(corrupt("invalid attestation subject kind"));
}

auto encode_platform(const platform& value) -> cbor::value {
    return map({{"os", cbor::value{value.os}}, {"arch", cbor::value{value.arch}}});
}

auto decode_platform(const cbor::value& value) -> result<platform> {
    const auto* os_value = find(value, "os");
    const auto* arch_value = find(value, "arch");
    const auto* os = os_value ? std::get_if<std::string>(&os_value->data) : nullptr;
    const auto* arch = arch_value ? std::get_if<std::string>(&arch_value->data) : nullptr;
    if (!os || !arch)
        return std::unexpected(corrupt("invalid action platform"));
    return platform{*os, *arch};
}

auto decode_text_array(const cbor::value* value, std::string_view field)
    -> result<std::vector<std::string>> {
    const auto* items = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!items)
        return std::unexpected(corrupt("invalid " + std::string{field}));
    auto output = std::vector<std::string>{};
    output.reserve(items->size());
    for (const auto& item : *items) {
        const auto* text = std::get_if<std::string>(&item.data);
        if (!text)
            return std::unexpected(corrupt("invalid " + std::string{field}));
        output.push_back(*text);
    }
    return output;
}

auto milliseconds_value(const cbor::value& value) -> result<std::chrono::milliseconds> {
    const auto* count = std::get_if<std::uint64_t>(&value.data);
    if (!count || *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::unexpected(corrupt("invalid action timing"));
    return std::chrono::milliseconds{static_cast<std::int64_t>(*count)};
}

auto encode_change_ref(const change_ref& value) -> cbor::value {
    return map({{"repo", encode_node_id(value.repo_id)}, {"id", encode_change_id(value.id)}});
}

auto decode_change_ref(const cbor::value& value) -> result<change_ref> {
    const auto* repository = find(value, "repo");
    const auto* id = find(value, "id");
    if (!repository || !id)
        return std::unexpected(corrupt("change reference is missing fields"));
    auto decoded_repository = decode_node_id(*repository);
    auto decoded_id = decode_change_id(*id);
    if (!decoded_repository || !decoded_id)
        return std::unexpected(corrupt("invalid change reference"));
    return change_ref{*decoded_repository, *decoded_id};
}

auto encode_repo_ref(const repo_ref& value) -> cbor::value {
    return map({{"repo", encode_node_id(value.repo_id)}, {"id", encode_change_id(value.id)}});
}

auto decode_repo_ref(const cbor::value& value) -> result<repo_ref> {
    const auto* repository = find(value, "repo");
    const auto* id = find(value, "id");
    if (!repository || !id)
        return std::unexpected(corrupt("repository reference is missing fields"));
    auto decoded_repository = decode_node_id(*repository);
    auto decoded_id = decode_change_id(*id);
    if (!decoded_repository || !decoded_id)
        return std::unexpected(corrupt("invalid repository reference"));
    return repo_ref{*decoded_repository, *decoded_id};
}

auto decode_action_timing(const cbor::value& value) -> result<action_timing> {
    const auto* queued_value = find(value, "queued_ms");
    const auto* sandbox_value = find(value, "sandbox_ms");
    const auto* exec_value = find(value, "exec_ms");
    const auto* hash_value = find(value, "hash_ms");
    const auto* store_value = find(value, "store_ms");
    if (!queued_value || !sandbox_value || !exec_value || !hash_value || !store_value) {
        return std::unexpected(corrupt("incomplete action timing"));
    }
    auto queued = milliseconds_value(*queued_value);
    auto sandbox = milliseconds_value(*sandbox_value);
    auto exec = milliseconds_value(*exec_value);
    auto hash = milliseconds_value(*hash_value);
    auto store = milliseconds_value(*store_value);
    if (!queued || !sandbox || !exec || !hash || !store) {
        return std::unexpected(corrupt("invalid action timing"));
    }
    return action_timing{*queued, *sandbox, *exec, *hash, *store};
}

} // namespace

auto digest_of(algorithm hash_algorithm, std::span<const std::byte> bytes) -> result<digest> {
    return hash(hash_algorithm, bytes);
}

auto encode(const blob& value) -> result<std::vector<std::byte>> {
    auto chunks = cbor::value::array{};
    for (const auto& chunk : value.chunks)
        chunks.push_back(encode_digest(chunk));
    auto sizes = value.chunk_sizes;
    if (sizes.empty() && value.chunks.size() == 1)
        sizes.push_back(value.size);
    const auto total = std::accumulate(sizes.begin(), sizes.end(), std::uint64_t{});
    if (sizes.size() != value.chunks.size() || total != value.size)
        return std::unexpected(
            error{errc::invalid_argument, "blob chunk sizes do not match content size", "blob"});
    auto chunk_sizes = cbor::value::array{};
    for (const auto size : sizes)
        chunk_sizes.push_back(cbor::value{size});
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "blob"},
                             {"size", cbor::value{value.size}},
                             {"chunks", std::move(chunks)},
                             {"chunk_sizes", std::move(chunk_sizes)}}));
}

auto encode(const tree& value) -> result<std::vector<std::byte>> {
    auto entries = value.entries;
    std::ranges::sort(entries, {}, &tree_entry::name);
    if (std::ranges::adjacent_find(entries, {}, &tree_entry::name) != entries.end())
        return std::unexpected(error{errc::invalid_argument, "duplicate tree entry", "tree"});
    auto output = cbor::value::array{};
    for (const auto& entry : entries) {
        auto fields =
            cbor::value::map{{"name", cbor::value{entry.name}},
                             {"mode", cbor::value{std::uint64_t{entry.mode}}},
                             {"kind", cbor::value{static_cast<std::uint64_t>(entry.kind)}},
                             {"id", encode_digest(entry.id)},
                             {"node", encode_node_id(entry.node)},
                             {"ref", encode_digest(entry.ref)}};
        if (entry.nested)
            fields.emplace_back("nested", encode_repo_ref(*entry.nested));
        output.push_back(cbor::value{std::move(fields)});
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "tree"},
                             {"entries", cbor::value{std::move(output)}}}));
}

auto encode_change_body_value(const change& value, bool include_signature)
    -> result<std::vector<std::byte>> {
    auto parents = cbor::value::array{};
    for (const auto& parent : value.parents)
        parents.push_back(encode_change_ref(parent));
    auto metadata = cbor::value::map{};
    for (const auto& [key, value] : value.metadata)
        metadata.emplace_back(cbor::value{key}, cbor::value{value});
    auto conflicts = cbor::value::array{};
    for (const auto& item : value.conflicts) {
        auto refs = cbor::value::array{};
        for (const auto& reference : item.refs)
            refs.push_back(encode_digest(reference));
        conflicts.push_back(map({{"path", cbor::value{item.path}}, {"refs", std::move(refs)}}));
    }
    auto fields = cbor::value::map{
        {"v", cbor::value{std::uint64_t{1}}},
        {"kind", "change"},
        {"parents", std::move(parents)},
        {"author", cbor::value{value.author.taap_id}},
        {"at", cbor::value{value.at.time_since_epoch().count()}},
        {"message", cbor::value{value.message}},
        {"root", encode_digest(value.root)},
        {"metadata", cbor::value{std::move(metadata)}},
        {"conflicts", std::move(conflicts)},
        {"symbols", value.symbols ? encode_digest(*value.symbols) : cbor::value{cbor::null{}}}};
    if (include_signature) {
        fields.emplace_back("sig", cbor::value{value.sig});
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto encode_change_body(const change& value) -> result<std::vector<std::byte>> {
    return encode_change_body_value(value, false);
}

auto encode(const change& value) -> result<std::vector<std::byte>> {
    return encode_change_body_value(value, true);
}

auto encode(const repo_record& value) -> result<std::vector<std::byte>> {
    auto root =
        value.root_change ? encode_change_id(value.root_change->id) : cbor::value{cbor::null{}};
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "repo"},
                             {"repo_id", encode_node_id(value.repo_id)},
                             {"name", cbor::value{value.name}},
                             {"created", cbor::value{value.created}},
                             {"root_change", std::move(root)}}));
}

auto encode_attestation_value(const attestation& value,
                              bool include_signature,
                              bool validate_schema) -> result<std::vector<std::byte>> {
    if (validate_schema) {
        auto schema = attestation_profile_registry::global().validate(value);
        if (!schema) {
            return std::unexpected(schema.error());
        }
    }
    auto detectors = cbor::value::array{};
    for (const auto& detector : value.detectors) {
        detectors.push_back(cbor::value{detector});
    }
    auto allowances = cbor::value::array{};
    for (const auto& allowance : value.allowances) {
        allowances.push_back(map({{"path", cbor::value{allowance.path}},
                                  {"detector", cbor::value{allowance.detector}},
                                  {"line", cbor::value{allowance.line}},
                                  {"reason", cbor::value{allowance.reason}}}));
    }
    auto fields = cbor::value::map{{"v", cbor::value{std::uint64_t{1}}},
                                   {"kind", "attestation"},
                                   {"subject", encode_digest(value.subject)},
                                   {"profile", cbor::value{value.profile}},
                                   {"detectors", cbor::value{std::move(detectors)}},
                                   {"findings", cbor::value{value.findings_count}},
                                   {"at", cbor::value{value.at.time_since_epoch().count()}},
                                   {"issuer", encode_actor(value.issuer)},
                                   {"allowances", cbor::value{std::move(allowances)}}};
    if (include_signature) {
        fields.emplace_back("sig", cbor::value{value.signature});
    }
    if (value.profile != "scan_result/1") {
        auto attributes = cbor::value::map{};
        if (value.profile == "slsa_provenance/1" && value.slsa_provenance) {
            auto predicate_value = encode_slsa_provenance(*value.slsa_provenance);
            const auto* predicate = std::get_if<cbor::value::map>(&predicate_value.data);
            attributes = *predicate;
        } else {
            for (const auto& attribute : value.attributes) {
                attributes.emplace_back(attribute.name, encode_attribute_value(attribute.value));
            }
        }
        auto commitments = cbor::value::map{};
        for (const auto& commitment : value.disclosure_commitments) {
            commitments.emplace_back(commitment.name, byte_string(commitment.value));
        }
        fields.emplace_back("subject_kind", std::string{to_string(value.subject_kind)});
        fields.emplace_back("attributes", cbor::value{std::move(attributes)});
        fields.emplace_back("attribute_disclosure_commitments",
                            cbor::value{std::move(commitments)});
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto encode_attestation_body(const attestation& value) -> result<std::vector<std::byte>> {
    return encode_attestation_value(value, false, true);
}

auto encode_attestation_body_unchecked(const attestation& value) -> result<std::vector<std::byte>> {
    return encode_attestation_value(value, false, false);
}

auto encode(const attestation& value) -> result<std::vector<std::byte>> {
    return encode_attestation_value(value, true, true);
}

auto encode(const action_record& value) -> result<std::vector<std::byte>> {
    auto command = cbor::value::array{};
    for (const auto& argument : value.input.command)
        command.emplace_back(argument);
    auto environment = cbor::value::map{};
    for (const auto& [name, setting] : value.input.env)
        environment.emplace_back(cbor::value{name}, cbor::value{setting});
    auto outputs = cbor::value::array{};
    for (const auto& path : value.input.outputs)
        outputs.emplace_back(path);
    auto fields = cbor::value::map{{"v", cbor::value{std::uint64_t{1}}},
                                   {"kind", "action"},
                                   {"inputs", encode_digest(value.input.inputs)},
                                   {"command", cbor::value{std::move(command)}},
                                   {"cwd", cbor::value{value.input.cwd}},
                                   {"env", cbor::value{std::move(environment)}},
                                   {"outputs", cbor::value{std::move(outputs)}},
                                   {"platform", encode_platform(value.input.platform)},
                                   {"toolchain", encode_digest(value.input.toolchain)},
                                   {"sandbox", encode_digest(value.sandbox)}};
    if (value.input.timeout) {
        if (value.input.timeout->count() < 0)
            return std::unexpected(
                error{errc::invalid_argument, "action timeout cannot be negative", "action"});
        fields.emplace_back("timeout_s",
                            cbor::value{static_cast<std::uint64_t>(value.input.timeout->count())});
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto encode_result_value(const action_result& value, bool include_signature)
    -> result<std::vector<std::byte>> {
    auto outputs = cbor::value::array{};
    for (const auto& output : value.outputs) {
        outputs.push_back(map({{"path", cbor::value{output.path}},
                               {"mode", cbor::value{static_cast<std::uint64_t>(output.mode)}},
                               {"blob", encode_digest(output.blob)}}));
    }
    auto manifest = cbor::value::array{};
    for (const auto& input : value.manifest) {
        manifest.push_back(
            map({{"path", cbor::value{input.path}}, {"digest", encode_digest(input.content)}}));
    }
    auto sboms = cbor::value::array{};
    for (const auto& sbom : value.sboms) {
        sboms.push_back(encode_digest(sbom));
    }
    const auto exit_value = value.exit < 0 ? cbor::value{static_cast<std::int64_t>(value.exit)}
                                           : cbor::value{static_cast<std::uint64_t>(value.exit)};
    if (value.record_version != 1 && value.record_version != 2) {
        return std::unexpected(corrupt("unsupported result record version"));
    }
    auto timing = cbor::value{};
    if (value.record_version == 1) {
        timing = map(
            {{"queued_ms", cbor::value{static_cast<std::uint64_t>(value.timing.queued.count())}},
             {"sandbox_ms", cbor::value{static_cast<std::uint64_t>(value.timing.sandbox.count())}},
             {"exec_ms", cbor::value{static_cast<std::uint64_t>(value.timing.exec.count())}},
             {"upload_ms", cbor::value{static_cast<std::uint64_t>(value.timing.store.count())}}});
    } else {
        timing = map(
            {{"queued_ms", cbor::value{static_cast<std::uint64_t>(value.timing.queued.count())}},
             {"sandbox_ms", cbor::value{static_cast<std::uint64_t>(value.timing.sandbox.count())}},
             {"exec_ms", cbor::value{static_cast<std::uint64_t>(value.timing.exec.count())}},
             {"hash_ms", cbor::value{static_cast<std::uint64_t>(value.timing.hash.count())}},
             {"store_ms", cbor::value{static_cast<std::uint64_t>(value.timing.store.count())}}});
    }
    auto fields = cbor::value::map{
        {"v", cbor::value{value.record_version}},
        {"kind", "result"},
        {"action", encode_digest(value.action)},
        {"exit", exit_value},
        {"outputs", cbor::value{std::move(outputs)}},
        {"stdout", encode_digest(value.stdout_output)},
        {"stderr", encode_digest(value.stderr_output)},
        {"manifest", cbor::value{std::move(manifest)}},
        {"timing", std::move(timing)},
        {"worker", cbor::value{value.worker}},
        {"observed_at",
         cbor::value{static_cast<std::int64_t>(value.observed_at.time_since_epoch().count())}},
        {"producer", encode_actor(value.producer)},
    };
    if (value.started_at) {
        fields.emplace_back(
            "started_at",
            cbor::value{static_cast<std::int64_t>(value.started_at->time_since_epoch().count())});
    }
    if (value.finished_at) {
        fields.emplace_back(
            "finished_at",
            cbor::value{static_cast<std::int64_t>(value.finished_at->time_since_epoch().count())});
    }
    if (value.provenance) {
        fields.emplace_back("provenance", encode_digest(*value.provenance));
    }
    if (!value.sboms.empty()) {
        fields.emplace_back("sboms", cbor::value{std::move(sboms)});
    }
    if (include_signature) {
        fields.emplace_back("sig", byte_string(value.sig));
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto encode(const action_result& value) -> result<std::vector<std::byte>> {
    return encode_result_value(value, true);
}

auto encode_result_body(const action_result& value) -> result<std::vector<std::byte>> {
    return encode_result_value(value, false);
}

auto encode(const sandbox_record& value) -> result<std::vector<std::byte>> {
    auto environment = cbor::value::array{};
    for (const auto& name : value.fixed_environment)
        environment.emplace_back(name);
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "sandbox"},
                             {"mechanism", cbor::value{value.mechanism}},
                             {"work_root", cbor::value{value.work_root}},
                             {"toolchain_root", cbor::value{value.toolchain_root}},
                             {"home_root", cbor::value{value.home_root}},
                             {"temporary_root", cbor::value{value.temporary_root}},
                             {"fixed_environment", cbor::value{std::move(environment)}},
                             {"network", cbor::value{value.network}}}));
}

auto encode(const action_index_root& value) -> result<std::vector<std::byte>> {
    auto entries = cbor::value::array{};
    for (const auto& entry : value.entries) {
        auto fields = cbor::value::map{{"candidate", encode_digest(entry.candidate)},
                                       {"result", encode_digest(entry.result)},
                                       {"sequence", cbor::value{entry.sequence}},
                                       {"divergent", cbor::value{entry.divergent}}};
        if (entry.verification) {
            fields.emplace_back("verification", encode_digest(*entry.verification));
        }
        entries.push_back(cbor::value{std::move(fields)});
    }
    auto traces = cbor::value::array{};
    for (const auto& trace : value.traces) {
        traces.push_back(encode_digest(trace));
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "action-index-root"},
                             {"entries", cbor::value{std::move(entries)}},
                             {"traces", cbor::value{std::move(traces)}}}));
}

auto encode(const build_trace& value) -> result<std::vector<std::byte>> {
    auto actions = cbor::value::array{};
    for (const auto& action : value.actions) {
        auto outputs = cbor::value::array{};
        for (const auto& output : action.outputs) {
            outputs.emplace_back(output);
        }
        actions.push_back(map(
            {{"result", encode_digest(action.result.value)},
             {"action", encode_digest(action.action)},
             {"outputs", cbor::value{std::move(outputs)}},
             {"cache", cbor::value{static_cast<std::uint64_t>(action.cache)}},
             {"start_ms", cbor::value{static_cast<std::uint64_t>(action.start_offset.count())}},
             {"timing",
              map({{"queued_ms",
                    cbor::value{static_cast<std::uint64_t>(action.timing.queued.count())}},
                   {"sandbox_ms",
                    cbor::value{static_cast<std::uint64_t>(action.timing.sandbox.count())}},
                   {"exec_ms", cbor::value{static_cast<std::uint64_t>(action.timing.exec.count())}},
                   {"hash_ms", cbor::value{static_cast<std::uint64_t>(action.timing.hash.count())}},
                   {"store_ms",
                    cbor::value{static_cast<std::uint64_t>(action.timing.store.count())}}})}}));
    }
    auto edges = cbor::value::array{};
    for (const auto& edge : value.edges) {
        edges.push_back(map({{"dependency", encode_digest(edge.dependency)},
                             {"dependent", encode_digest(edge.dependent)}}));
    }
    return cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}},
             {"kind", "build_trace"},
             {"wall_ms", cbor::value{static_cast<std::uint64_t>(value.wall_time.count())}},
             {"actions", cbor::value{std::move(actions)}},
             {"edges", cbor::value{std::move(edges)}},
             {"cache",
              map({{"hits", cbor::value{value.cache.hits}},
                   {"misses", cbor::value{value.cache.misses}},
                   {"bytes_served", cbor::value{value.cache.bytes_served}},
                   {"bytes_rebuilt", cbor::value{value.cache.bytes_rebuilt}},
                   {"bytes_avoided", cbor::value{value.cache.disk.bytes_avoided}}})}}));
}

auto encode(const sbom_record& value) -> result<std::vector<std::byte>> {
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "sbom"},
                             {"format", cbor::value{value.format}},
                             {"document", byte_string(value.document)}}));
}

auto decode_blob(std::span<const std::byte> bytes) -> result<blob> {
    auto source = decode(bytes, "blob");
    if (!source)
        return std::unexpected(source.error());
    const auto* size = find(*source, "size");
    const auto* chunks = find(*source, "chunks");
    const auto* chunk_sizes = find(*source, "chunk_sizes");
    const auto* n = size ? std::get_if<std::uint64_t>(&size->data) : nullptr;
    const auto* values = chunks ? std::get_if<cbor::value::array>(&chunks->data) : nullptr;
    if (!n || !values)
        return std::unexpected(corrupt("invalid blob record"));
    blob output{{}, *n, {}};
    for (const auto& item : *values) {
        auto digest = decode_digest(item);
        if (!digest)
            return std::unexpected(digest.error());
        output.chunks.push_back(*digest);
    }
    if (chunk_sizes) {
        const auto* sizes = std::get_if<cbor::value::array>(&chunk_sizes->data);
        if (!sizes || sizes->size() != output.chunks.size())
            return std::unexpected(corrupt("invalid blob chunk sizes"));
        for (const auto& item : *sizes) {
            const auto* value = std::get_if<std::uint64_t>(&item.data);
            if (!value)
                return std::unexpected(corrupt("invalid blob chunk size"));
            output.chunk_sizes.push_back(*value);
        }
    } else if (output.chunks.size() == 1) {
        output.chunk_sizes.push_back(*n);
    }
    const auto total =
        std::accumulate(output.chunk_sizes.begin(), output.chunk_sizes.end(), std::uint64_t{});
    if (output.chunk_sizes.size() != output.chunks.size() || total != *n)
        return std::unexpected(corrupt("blob chunk sizes do not match content size"));
    return output;
}

auto decode_tree(std::span<const std::byte> bytes) -> result<tree> {
    auto source = decode(bytes, "tree");
    if (!source)
        return std::unexpected(source.error());
    const auto* items = find(*source, "entries");
    const auto* values = items ? std::get_if<cbor::value::array>(&items->data) : nullptr;
    if (!values)
        return std::unexpected(corrupt("invalid tree record"));
    tree output{};
    for (const auto& item : *values) {
        const auto* name = find(item, "name");
        const auto* mode = find(item, "mode");
        const auto* kind = find(item, "kind");
        const auto* id = find(item, "id");
        const auto* node = find(item, "node");
        const auto* ref = find(item, "ref");
        const auto* text = name ? std::get_if<std::string>(&name->data) : nullptr;
        const auto* bits = mode ? std::get_if<std::uint64_t>(&mode->data) : nullptr;
        const auto* type = kind ? std::get_if<std::uint64_t>(&kind->data) : nullptr;
        if (!text || !bits || !type || *bits > std::numeric_limits<std::uint32_t>::max() ||
            *type > static_cast<std::uint64_t>(node_kind::repo))
            return std::unexpected(corrupt("invalid tree entry"));
        auto object =
            id ? decode_digest(*id) : result<digest>{std::unexpected(corrupt("tree id missing"))};
        auto identity = node ? decode_node_id(*node)
                             : result<node_id>{std::unexpected(corrupt("tree node missing"))};
        auto reference = ref ? decode_digest(*ref)
                             : result<digest>{std::unexpected(corrupt("tree ref missing"))};
        if (!object || !identity || !reference)
            return std::unexpected(corrupt("invalid tree entry references"));
        auto nested = std::optional<repo_ref>{};
        if (const auto* value = find(item, "nested")) {
            auto decoded = decode_repo_ref(*value);
            if (!decoded)
                return std::unexpected(decoded.error());
            nested = *decoded;
        }
        output.entries.push_back({*text,
                                  static_cast<std::uint32_t>(*bits),
                                  static_cast<node_kind>(*type),
                                  *object,
                                  *identity,
                                  *reference,
                                  nested});
    }
    if (!std::ranges::is_sorted(output.entries, {}, &tree_entry::name))
        return std::unexpected(corrupt("tree entries are not sorted"));
    return output;
}

auto decode_change(std::span<const std::byte> bytes, std::optional<algorithm> hash_algorithm)
    -> result<change> {
    auto source = decode(bytes, "change");
    if (!source)
        return std::unexpected(source.error());
    const auto* parents = find(*source, "parents");
    const auto* author = find(*source, "author");
    const auto* at = find(*source, "at");
    const auto* message = find(*source, "message");
    const auto* root = find(*source, "root");
    const auto* metadata = find(*source, "metadata");
    const auto* parent_values = parents ? std::get_if<cbor::value::array>(&parents->data) : nullptr;
    const auto* who = author ? std::get_if<std::string>(&author->data) : nullptr;
    const auto* signed_when = at ? std::get_if<std::int64_t>(&at->data) : nullptr;
    const auto* unsigned_when = at ? std::get_if<std::uint64_t>(&at->data) : nullptr;
    const auto* title = message ? std::get_if<std::string>(&message->data) : nullptr;
    if (!parent_values || !who ||
        (!signed_when &&
         (!unsigned_when || *unsigned_when > std::numeric_limits<std::int64_t>::max())) ||
        !title || !root)
        return std::unexpected(corrupt("invalid change record"));
    auto root_digest = decode_digest(*root);
    if (!root_digest)
        return std::unexpected(root_digest.error());
    const auto seconds = signed_when ? *signed_when : static_cast<std::int64_t>(*unsigned_when);
    change output{{},
                  {},
                  actor{*who},
                  *title,
                  std::chrono::sys_seconds{std::chrono::seconds{seconds}},
                  *root_digest,
                  {},
                  {},
                  {},
                  std::nullopt,
                  {}};
    for (const auto& item : *parent_values) {
        auto parent = decode_change_ref(item);
        if (!parent)
            return std::unexpected(parent.error());
        output.parents.push_back(*parent);
    }
    if (metadata) {
        const auto* fields = std::get_if<cbor::value::map>(&metadata->data);
        if (!fields)
            return std::unexpected(corrupt("invalid change metadata"));
        for (const auto& [key, value] : *fields) {
            const auto* name = std::get_if<std::string>(&key.data);
            const auto* text = std::get_if<std::string>(&value.data);
            if (!name || !text || !output.metadata.emplace(*name, *text).second)
                return std::unexpected(corrupt("invalid change metadata entry"));
        }
    }
    const auto* conflicts = find(*source, "conflicts");
    const auto* symbols = find(*source, "symbols");
    const auto* signature = find(*source, "sig");
    const auto* values = conflicts ? std::get_if<cbor::value::array>(&conflicts->data) : nullptr;
    const auto* sig = signature ? std::get_if<cbor::value::bytes>(&signature->data) : nullptr;
    if (!values || !symbols || !sig)
        return std::unexpected(corrupt("invalid change extensions"));
    for (const auto& item : *values) {
        const auto* path = find(item, "path");
        const auto* references = find(item, "refs");
        const auto* name = path ? std::get_if<std::string>(&path->data) : nullptr;
        const auto* refs =
            references ? std::get_if<cbor::value::array>(&references->data) : nullptr;
        if (!name || !refs)
            return std::unexpected(corrupt("invalid conflict value"));
        conflict value{*name, {}};
        for (const auto& reference : *refs) {
            auto decoded = decode_digest(reference);
            if (!decoded)
                return std::unexpected(decoded.error());
            value.refs.push_back(*decoded);
        }
        output.conflicts.push_back(std::move(value));
    }
    if (!std::holds_alternative<cbor::null>(symbols->data)) {
        auto symbol = decode_digest(*symbols);
        if (!symbol)
            return std::unexpected(symbol.error());
        output.symbols = *symbol;
    }
    output.sig = *sig;
    if (hash_algorithm) {
        auto digest = digest_of(*hash_algorithm, bytes);
        if (!digest)
            return std::unexpected(digest.error());
        output.id = {*digest};
    }
    return output;
}

auto decode_repo(std::span<const std::byte> bytes) -> result<repo_record> {
    auto source = decode(bytes, "repo");
    if (!source)
        return std::unexpected(source.error());
    const auto* id = find(*source, "repo_id");
    const auto* name = find(*source, "name");
    const auto* created = find(*source, "created");
    const auto* title = name ? std::get_if<std::string>(&name->data) : nullptr;
    const auto* when = created ? std::get_if<std::int64_t>(&created->data) : nullptr;
    if (!id || !title || !when)
        return std::unexpected(corrupt("invalid repo record"));
    auto identity = decode_node_id(*id);
    if (!identity)
        return std::unexpected(identity.error());
    return repo_record{*identity, *title, *when, std::nullopt};
}

auto decode_attestation(std::span<const std::byte> bytes) -> result<attestation> {
    auto source = decode(bytes, "attestation");
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto* subject_value = find(*source, "subject");
    const auto* profile_value = find(*source, "profile");
    const auto* at_value = find(*source, "at");
    const auto* issuer_value = find(*source, "issuer");
    const auto* signature_value = find(*source, "sig");
    const auto* profile = profile_value ? std::get_if<std::string>(&profile_value->data) : nullptr;
    const auto* signature =
        signature_value ? std::get_if<cbor::value::bytes>(&signature_value->data) : nullptr;
    auto at = std::int64_t{};
    if (const auto* signed_at = at_value ? std::get_if<std::int64_t>(&at_value->data) : nullptr) {
        at = *signed_at;
    } else if (const auto* unsigned_at =
                   at_value ? std::get_if<std::uint64_t>(&at_value->data) : nullptr;
               unsigned_at && *unsigned_at <= static_cast<std::uint64_t>(
                                                  std::numeric_limits<std::int64_t>::max())) {
        at = static_cast<std::int64_t>(*unsigned_at);
    } else {
        return std::unexpected(corrupt("invalid attestation timestamp"));
    }
    if (!subject_value || !profile || !issuer_value || !signature) {
        return std::unexpected(corrupt("invalid attestation record"));
    }
    auto subject = decode_digest(*subject_value);
    auto issuer = decode_actor(*issuer_value);
    if (!subject || !issuer) {
        return std::unexpected(corrupt("invalid attestation identity"));
    }
    attestation output{.subject = *subject,
                       .profile = *profile,
                       .at = std::chrono::sys_seconds{std::chrono::seconds{at}},
                       .issuer = std::move(*issuer),
                       .signature = *signature};
    if (output.profile != "scan_result/1") {
        const auto* subject_kind_value = find(*source, "subject_kind");
        const auto* attributes_value = find(*source, "attributes");
        const auto* commitments_value = find(*source, "attribute_disclosure_commitments");
        const auto* subject_kind =
            subject_kind_value ? std::get_if<std::string>(&subject_kind_value->data) : nullptr;
        const auto* attributes =
            attributes_value ? std::get_if<cbor::value::map>(&attributes_value->data) : nullptr;
        const auto* commitments =
            commitments_value ? std::get_if<cbor::value::map>(&commitments_value->data) : nullptr;
        if (!subject_kind || !attributes || !commitments) {
            return std::unexpected(corrupt("invalid generic attestation record"));
        }
        auto parsed_kind = parse_subject_kind(*subject_kind);
        if (!parsed_kind) {
            return std::unexpected(parsed_kind.error());
        }
        output.subject_kind = *parsed_kind;
        if (output.profile == "slsa_provenance/1") {
            auto predicate = decode_slsa_provenance(*attributes_value);
            if (!predicate || !commitments->empty()) {
                return std::unexpected(corrupt("invalid SLSA provenance attestation"));
            }
            output.slsa_provenance = std::move(*predicate);
            return output;
        }
        for (const auto& [key, attribute_value] : *attributes) {
            const auto* name = std::get_if<std::string>(&key.data);
            if (!name) {
                return std::unexpected(corrupt("invalid attestation attribute name"));
            }
            auto decoded = decode_attribute_value(attribute_value);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            output.attributes.push_back({*name, std::move(*decoded)});
        }
        for (const auto& [key, commitment_value] : *commitments) {
            const auto* name = std::get_if<std::string>(&key.data);
            const auto* commitment = std::get_if<cbor::value::bytes>(&commitment_value.data);
            if (!name || !commitment) {
                return std::unexpected(corrupt("invalid attestation commitment"));
            }
            output.disclosure_commitments.push_back({*name, *commitment});
        }
        return output;
    }
    const auto* detectors_value = find(*source, "detectors");
    const auto* findings_value = find(*source, "findings");
    const auto* allowances_value = find(*source, "allowances");
    const auto* detectors =
        detectors_value ? std::get_if<cbor::value::array>(&detectors_value->data) : nullptr;
    const auto* findings =
        findings_value ? std::get_if<std::uint64_t>(&findings_value->data) : nullptr;
    const auto* allowances =
        allowances_value ? std::get_if<cbor::value::array>(&allowances_value->data) : nullptr;
    if (!detectors || !findings || !allowances) {
        return std::unexpected(corrupt("invalid scan attestation record"));
    }
    output.findings_count = *findings;
    for (const auto& detector : *detectors) {
        const auto* id = std::get_if<std::string>(&detector.data);
        if (!id) {
            return std::unexpected(corrupt("invalid attestation detector"));
        }
        output.detectors.push_back(*id);
    }
    for (const auto& allowance : *allowances) {
        const auto* path_value = find(allowance, "path");
        const auto* detector_value = find(allowance, "detector");
        const auto* line_value = find(allowance, "line");
        const auto* reason_value = find(allowance, "reason");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        const auto* detector =
            detector_value ? std::get_if<std::string>(&detector_value->data) : nullptr;
        const auto* line = line_value ? std::get_if<std::uint64_t>(&line_value->data) : nullptr;
        const auto* reason = reason_value ? std::get_if<std::string>(&reason_value->data) : nullptr;
        if (!path || !detector || !line || !reason) {
            return std::unexpected(corrupt("invalid attestation allowance"));
        }
        output.allowances.push_back({*path, *detector, *line, *reason});
    }
    return output;
}

auto decode_action(std::span<const std::byte> bytes) -> result<action_record> {
    auto source = decode(bytes, "action");
    if (!source)
        return std::unexpected(source.error());
    auto command = decode_text_array(find(*source, "command"), "action command");
    auto outputs = decode_text_array(find(*source, "outputs"), "action outputs");
    const auto* cwd_value = find(*source, "cwd");
    const auto* environment_value = find(*source, "env");
    const auto* platform_value = find(*source, "platform");
    const auto* cwd = cwd_value ? std::get_if<std::string>(&cwd_value->data) : nullptr;
    const auto* environment =
        environment_value ? std::get_if<cbor::value::map>(&environment_value->data) : nullptr;
    auto platform_result =
        platform_value ? decode_platform(*platform_value)
                       : result<platform>{std::unexpected(corrupt("action platform is missing"))};
    const auto* inputs_value = find(*source, "inputs");
    const auto* toolchain_value = find(*source, "toolchain");
    const auto* sandbox_value = find(*source, "sandbox");
    auto inputs = inputs_value ? decode_digest(*inputs_value)
                               : result<digest>{std::unexpected(corrupt("action inputs missing"))};
    auto toolchain = toolchain_value
                         ? decode_digest(*toolchain_value)
                         : result<digest>{std::unexpected(corrupt("action toolchain missing"))};
    auto sandbox = sandbox_value
                       ? decode_digest(*sandbox_value)
                       : result<digest>{std::unexpected(corrupt("action sandbox missing"))};
    if (!command || !outputs || !cwd || !environment || !platform_result || !inputs || !toolchain ||
        !sandbox)
        return std::unexpected(corrupt("invalid action record"));
    auto env = std::map<std::string, std::string>{};
    for (const auto& [name_value, setting_value] : *environment) {
        const auto* name = std::get_if<std::string>(&name_value.data);
        const auto* setting = std::get_if<std::string>(&setting_value.data);
        if (!name || !setting || !env.emplace(*name, *setting).second)
            return std::unexpected(corrupt("invalid action environment"));
    }
    auto timeout = std::optional<std::chrono::seconds>{};
    if (const auto* timeout_value = find(*source, "timeout_s")) {
        const auto* count = std::get_if<std::uint64_t>(&timeout_value->data);
        if (!count || *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::unexpected(corrupt("invalid action timeout"));
        timeout = std::chrono::seconds{static_cast<std::int64_t>(*count)};
    }
    return action_record{action_input{std::move(*command),
                                      *cwd,
                                      std::move(env),
                                      std::move(*outputs),
                                      *inputs,
                                      *toolchain,
                                      std::move(*platform_result),
                                      timeout},
                         *sandbox};
}

auto decode_result(std::span<const std::byte> bytes) -> result<action_result> {
    auto source = cbor::decode(bytes);
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto* version_value = find(*source, "v");
    const auto* kind_value = find(*source, "kind");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    if (!version || (*version != 1 && *version != 2) || !kind || *kind != "result") {
        return std::unexpected(corrupt("unsupported result record version"));
    }
    const auto* action_value = find(*source, "action");
    const auto* stdout_value = find(*source, "stdout");
    const auto* stderr_value = find(*source, "stderr");
    auto action = action_value ? decode_digest(*action_value)
                               : result<digest>{std::unexpected(corrupt("result action missing"))};
    auto stdout_output = stdout_value
                             ? decode_digest(*stdout_value)
                             : result<digest>{std::unexpected(corrupt("result stdout missing"))};
    auto stderr_output = stderr_value
                             ? decode_digest(*stderr_value)
                             : result<digest>{std::unexpected(corrupt("result stderr missing"))};
    const auto* exit_value = find(*source, "exit");
    auto exit = std::optional<int>{};
    if (const auto* signed_exit =
            exit_value ? std::get_if<std::int64_t>(&exit_value->data) : nullptr;
        signed_exit && *signed_exit >= std::numeric_limits<int>::min() &&
        *signed_exit <= std::numeric_limits<int>::max()) {
        exit = static_cast<int>(*signed_exit);
    } else if (const auto* unsigned_exit =
                   exit_value ? std::get_if<std::uint64_t>(&exit_value->data) : nullptr;
               unsigned_exit &&
               *unsigned_exit <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        exit = static_cast<int>(*unsigned_exit);
    }
    const auto* output_values = find(*source, "outputs");
    const auto* manifest_values = find(*source, "manifest");
    const auto* timing_value = find(*source, "timing");
    const auto* worker_value = find(*source, "worker");
    const auto* observed_value = find(*source, "observed_at");
    const auto* producer_value = find(*source, "producer");
    const auto* signature_value = find(*source, "sig");
    const auto* started_value = find(*source, "started_at");
    const auto* finished_value = find(*source, "finished_at");
    const auto* provenance_value = find(*source, "provenance");
    const auto* sboms_value = find(*source, "sboms");
    const auto* outputs =
        output_values ? std::get_if<cbor::value::array>(&output_values->data) : nullptr;
    const auto* manifest =
        manifest_values ? std::get_if<cbor::value::array>(&manifest_values->data) : nullptr;
    const auto* worker = worker_value ? std::get_if<std::string>(&worker_value->data) : nullptr;
    auto producer = producer_value
                        ? decode_actor(*producer_value)
                        : result<actor>{std::unexpected(corrupt("result producer missing"))};
    const auto* signature =
        signature_value ? std::get_if<cbor::value::bytes>(&signature_value->data) : nullptr;
    auto observed = std::optional<std::int64_t>{};
    if (const auto* signed_observed =
            observed_value ? std::get_if<std::int64_t>(&observed_value->data) : nullptr) {
        observed = *signed_observed;
    } else if (const auto* unsigned_observed =
                   observed_value ? std::get_if<std::uint64_t>(&observed_value->data) : nullptr;
               unsigned_observed &&
               *unsigned_observed <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        observed = static_cast<std::int64_t>(*unsigned_observed);
    }
    if (!action || !stdout_output || !stderr_output || !exit || !outputs || !manifest ||
        !timing_value || !worker || !observed || !producer || !signature)
        return std::unexpected(corrupt("invalid result record"));
    action_result output{.action = *action,
                         .exit = *exit,
                         .stdout_output = *stdout_output,
                         .stderr_output = *stderr_output,
                         .worker = *worker,
                         .observed_at = std::chrono::sys_seconds{std::chrono::seconds{*observed}},
                         .producer = std::move(*producer),
                         .sig = *signature};
    const auto decode_time =
        [](const cbor::value* value,
           std::string_view field) -> result<std::optional<std::chrono::sys_seconds>> {
        if (!value) {
            return std::nullopt;
        }
        if (const auto* signed_value = std::get_if<std::int64_t>(&value->data)) {
            return std::chrono::sys_seconds{std::chrono::seconds{*signed_value}};
        }
        if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value->data);
            unsigned_value && *unsigned_value <= static_cast<std::uint64_t>(
                                                     std::numeric_limits<std::int64_t>::max())) {
            return std::chrono::sys_seconds{
                std::chrono::seconds{static_cast<std::int64_t>(*unsigned_value)}};
        }
        return std::unexpected(corrupt("invalid result " + std::string{field}));
    };
    auto started = decode_time(started_value, "started time");
    auto finished = decode_time(finished_value, "finished time");
    if (!started || !finished) {
        return std::unexpected(corrupt("invalid result execution timestamps"));
    }
    output.started_at = *started;
    output.finished_at = *finished;
    if (provenance_value) {
        auto provenance = decode_digest(*provenance_value);
        if (!provenance) {
            return std::unexpected(corrupt("invalid result provenance reference"));
        }
        output.provenance = *provenance;
    }
    if (sboms_value) {
        const auto* sboms = std::get_if<cbor::value::array>(&sboms_value->data);
        if (!sboms) {
            return std::unexpected(corrupt("invalid result SBOM references"));
        }
        for (const auto& item : *sboms) {
            auto sbom = decode_digest(item);
            if (!sbom) {
                return std::unexpected(corrupt("invalid result SBOM reference"));
            }
            output.sboms.push_back(*sbom);
        }
    }
    for (const auto& item : *outputs) {
        const auto* path_value = find(item, "path");
        const auto* mode_value = find(item, "mode");
        const auto* blob_value = find(item, "blob");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        const auto* mode = mode_value ? std::get_if<std::uint64_t>(&mode_value->data) : nullptr;
        auto blob = blob_value ? decode_digest(*blob_value)
                               : result<digest>{std::unexpected(corrupt("output blob missing"))};
        if (!path || !mode || *mode > std::numeric_limits<std::uint32_t>::max() || !blob)
            return std::unexpected(corrupt("invalid result output"));
        output.outputs.push_back({*path, static_cast<std::uint32_t>(*mode), *blob});
    }
    for (const auto& item : *manifest) {
        const auto* path_value = find(item, "path");
        const auto* digest_value = find(item, "digest");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        auto content = digest_value
                           ? decode_digest(*digest_value)
                           : result<digest>{std::unexpected(corrupt("manifest digest missing"))};
        if (!path || !content)
            return std::unexpected(corrupt("invalid result manifest"));
        output.manifest.push_back({*path, *content});
    }
    const auto* queued_value = find(*timing_value, "queued_ms");
    const auto* sandbox_timing_value = find(*timing_value, "sandbox_ms");
    const auto* exec_value = find(*timing_value, "exec_ms");
    const auto* hash_value = find(*timing_value, "hash_ms");
    const auto* store_value = find(*timing_value, "store_ms");
    const auto* upload_value = find(*timing_value, "upload_ms");
    auto queued =
        queued_value
            ? milliseconds_value(*queued_value)
            : result<std::chrono::milliseconds>{std::unexpected(corrupt("queued timing missing"))};
    auto sandbox_timing =
        sandbox_timing_value
            ? milliseconds_value(*sandbox_timing_value)
            : result<std::chrono::milliseconds>{std::unexpected(corrupt("sandbox timing missing"))};
    auto exec =
        exec_value
            ? milliseconds_value(*exec_value)
            : result<std::chrono::milliseconds>{std::unexpected(corrupt("exec timing missing"))};
    auto hash = hash_value ? milliseconds_value(*hash_value)
                           : result<std::chrono::milliseconds>{std::chrono::milliseconds{}};
    auto store =
        store_value ? milliseconds_value(*store_value)
        : upload_value
            ? milliseconds_value(*upload_value)
            : result<std::chrono::milliseconds>{std::unexpected(corrupt("store timing missing"))};
    if (!queued || !sandbox_timing || !exec || !hash || !store) {
        return std::unexpected(corrupt("invalid result timing"));
    }
    if (*version == 2 && (!hash_value || !store_value)) {
        return std::unexpected(corrupt("version 2 result timing is incomplete"));
    }
    output.timing = {*queued, *sandbox_timing, *exec, *hash, *store};
    output.record_version = *version;
    return output;
}

auto decode_action_index_root(std::span<const std::byte> bytes) -> result<action_index_root> {
    auto source = decode(bytes, "action-index-root");
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto* entries_value = find(*source, "entries");
    const auto* entries =
        entries_value ? std::get_if<cbor::value::array>(&entries_value->data) : nullptr;
    if (!entries) {
        return std::unexpected(corrupt("invalid action index root"));
    }
    auto output = action_index_root{};
    output.entries.reserve(entries->size());
    for (const auto& item : *entries) {
        const auto* candidate_value = find(item, "candidate");
        const auto* result_value = find(item, "result");
        const auto* sequence_value = find(item, "sequence");
        const auto* divergent_value = find(item, "divergent");
        const auto* verification_value = find(item, "verification");
        auto candidate = candidate_value
                             ? decode_digest(*candidate_value)
                             : result<digest>{std::unexpected(corrupt("candidate missing"))};
        auto result_id = result_value ? decode_digest(*result_value)
                                      : result<digest>{std::unexpected(corrupt("result missing"))};
        const auto* sequence =
            sequence_value ? std::get_if<std::uint64_t>(&sequence_value->data) : nullptr;
        const auto* divergent =
            divergent_value ? std::get_if<bool>(&divergent_value->data) : nullptr;
        if (divergent_value && !divergent) {
            return std::unexpected(corrupt("invalid action divergence flag"));
        }
        auto verification = std::optional<digest>{};
        if (verification_value) {
            auto decoded = decode_digest(*verification_value);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            verification = *decoded;
        }
        if (!candidate || !result_id || !sequence || *sequence == 0) {
            return std::unexpected(corrupt("invalid action index entry"));
        }
        output.entries.push_back(
            {*candidate, *result_id, *sequence, divergent ? *divergent : false, verification});
    }
    if (const auto* traces_value = find(*source, "traces")) {
        const auto* traces = std::get_if<cbor::value::array>(&traces_value->data);
        if (!traces) {
            return std::unexpected(corrupt("invalid action index trace list"));
        }
        output.traces.reserve(traces->size());
        for (const auto& item : *traces) {
            auto trace = decode_digest(item);
            if (!trace) {
                return std::unexpected(trace.error());
            }
            output.traces.push_back(*trace);
        }
    }
    return output;
}

auto decode_build_trace(std::span<const std::byte> bytes) -> result<build_trace> {
    auto source = decode(bytes, "build_trace");
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto* wall_value = find(*source, "wall_ms");
    const auto* actions_value = find(*source, "actions");
    const auto* edges_value = find(*source, "edges");
    const auto* cache_value = find(*source, "cache");
    auto wall = wall_value ? milliseconds_value(*wall_value)
                           : result<std::chrono::milliseconds>{
                                 std::unexpected(corrupt("build trace wall time missing"))};
    const auto* actions =
        actions_value ? std::get_if<cbor::value::array>(&actions_value->data) : nullptr;
    const auto* edges = edges_value ? std::get_if<cbor::value::array>(&edges_value->data) : nullptr;
    if (!wall || !actions || !edges || !cache_value) {
        return std::unexpected(corrupt("invalid build trace"));
    }
    auto output = build_trace{.wall_time = *wall};
    output.actions.reserve(actions->size());
    for (const auto& item : *actions) {
        const auto* result_value = find(item, "result");
        const auto* action_value = find(item, "action");
        const auto* outputs_value = find(item, "outputs");
        const auto* timing_value = find(item, "timing");
        const auto* cache_status_value = find(item, "cache");
        const auto* start_value = find(item, "start_ms");
        auto result_digest = result_value
                                 ? decode_digest(*result_value)
                                 : result<digest>{std::unexpected(corrupt("trace result missing"))};
        auto action = action_value
                          ? decode_digest(*action_value)
                          : result<digest>{std::unexpected(corrupt("trace action missing"))};
        auto outputs = decode_text_array(outputs_value, "trace outputs");
        auto timing = timing_value
                          ? decode_action_timing(*timing_value)
                          : result<action_timing>{std::unexpected(corrupt("trace timing missing"))};
        const auto* status =
            cache_status_value ? std::get_if<std::uint64_t>(&cache_status_value->data) : nullptr;
        auto start = start_value ? milliseconds_value(*start_value)
                                 : result<std::chrono::milliseconds>{
                                       std::unexpected(corrupt("trace start missing"))};
        if (!result_digest || !action || !outputs || !timing || !status ||
            *status > static_cast<std::uint64_t>(cache_status::divergent) || !start) {
            return std::unexpected(corrupt("invalid build trace action"));
        }
        output.actions.push_back({result_id{*result_digest},
                                  *action,
                                  std::move(*outputs),
                                  *timing,
                                  static_cast<cache_status>(*status),
                                  *start});
    }
    output.edges.reserve(edges->size());
    for (const auto& item : *edges) {
        const auto* dependency_value = find(item, "dependency");
        const auto* dependent_value = find(item, "dependent");
        auto dependency =
            dependency_value ? decode_digest(*dependency_value)
                             : result<digest>{std::unexpected(corrupt("trace dependency missing"))};
        auto dependent = dependent_value
                             ? decode_digest(*dependent_value)
                             : result<digest>{std::unexpected(corrupt("trace dependent missing"))};
        if (!dependency || !dependent) {
            return std::unexpected(corrupt("invalid build trace edge"));
        }
        output.edges.push_back({*dependency, *dependent});
    }
    const auto read_count = [&](std::string_view name) -> result<std::uint64_t> {
        const auto* value = find(*cache_value, name);
        const auto* count = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
        if (!count) {
            return std::unexpected(corrupt("build trace cache field missing"));
        }
        return *count;
    };
    auto hits = read_count("hits");
    auto misses = read_count("misses");
    auto bytes_served = read_count("bytes_served");
    auto bytes_rebuilt = read_count("bytes_rebuilt");
    auto bytes_avoided = read_count("bytes_avoided");
    if (!hits || !misses || !bytes_served || !bytes_rebuilt || !bytes_avoided) {
        return std::unexpected(corrupt("invalid build trace cache outcome"));
    }
    output.cache.hits = *hits;
    output.cache.misses = *misses;
    output.cache.bytes_served = *bytes_served;
    output.cache.bytes_rebuilt = *bytes_rebuilt;
    output.cache.disk.available = true;
    output.cache.disk.bytes_avoided = *bytes_avoided;
    return output;
}

auto decode_sbom(std::span<const std::byte> bytes) -> result<sbom_record> {
    auto source = decode(bytes, "sbom");
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto* format_value = find(*source, "format");
    const auto* document_value = find(*source, "document");
    const auto* format = format_value ? std::get_if<std::string>(&format_value->data) : nullptr;
    const auto* document =
        document_value ? std::get_if<cbor::value::bytes>(&document_value->data) : nullptr;
    if (!format || !document) {
        return std::unexpected(corrupt("invalid SBOM record"));
    }
    return sbom_record{*format, *document};
}

} // namespace liquid::objects
