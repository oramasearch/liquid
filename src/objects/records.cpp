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
        output.push_back(map({{"name", cbor::value{entry.name}},
                              {"mode", cbor::value{std::uint64_t{entry.mode}}},
                              {"kind", cbor::value{static_cast<std::uint64_t>(entry.kind)}},
                              {"id", encode_digest(entry.id)},
                              {"node", encode_node_id(entry.node)},
                              {"ref", encode_digest(entry.ref)}}));
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", "tree"},
                             {"entries", cbor::value{std::move(output)}}}));
}

auto encode_change_body_value(const change& value, bool include_signature)
    -> result<std::vector<std::byte>> {
    auto parents = cbor::value::array{};
    for (const auto& parent : value.parents)
        parents.push_back(encode_change_id(parent));
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
        for (const auto& attribute : value.attributes) {
            attributes.emplace_back(attribute.name, encode_attribute_value(attribute.value));
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
        output.entries.push_back({*text,
                                  static_cast<std::uint32_t>(*bits),
                                  static_cast<node_kind>(*type),
                                  *object,
                                  *identity,
                                  *reference});
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
                  std::nullopt,
                  {}};
    for (const auto& item : *parent_values) {
        auto parent = decode_change_id(item);
        if (!parent)
            return std::unexpected(parent.error());
        output.parents.push_back(*parent);
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

} // namespace liquid::objects
