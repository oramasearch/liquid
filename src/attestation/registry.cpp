#include "liquid/attestation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>
#include <type_traits>
#include <utility>

#include "encoding/cbor.hpp"
#include "identity/taap.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto invalid(std::string reason) -> error {
    return {errc::invalid_argument, std::move(reason), "attestation"};
}

auto attribute_value(const attestation_value& value) -> cbor::value {
    return std::visit(
        [](const auto& item) -> cbor::value {
            using value_type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<value_type, std::vector<std::string>>) {
                auto values = cbor::value::array{};
                for (const auto& entry : item) {
                    values.emplace_back(entry);
                }
                return cbor::value{std::move(values)};
            } else {
                return cbor::value{item};
            }
        },
        value);
}

auto is_type(const attestation_value& value, attestation_field_type type) -> bool {
    switch (type) {
    case attestation_field_type::text:
        return std::holds_alternative<std::string>(value);
    case attestation_field_type::unsigned_integer:
        return std::holds_alternative<std::uint64_t>(value);
    case attestation_field_type::boolean:
        return std::holds_alternative<bool>(value);
    case attestation_field_type::text_list:
        return std::holds_alternative<std::vector<std::string>>(value);
    }
    return false;
}

auto field(std::string name, attestation_field_type type, bool required, bool may_commit = false)
    -> attestation_profile_field {
    return {std::move(name), type, required, may_commit};
}

auto profile(std::string name,
             std::vector<attestation_profile_field> schema,
             std::vector<attestation_subject_kind> subject_kinds,
             attestation_status_kind status,
             std::chrono::seconds maximum_age = {}) -> attestation_profile {
    return {std::move(name), std::move(schema), std::move(subject_kinds), {status, maximum_age}};
}

auto validate_commitments(const attestation& value, const attestation_profile& profile)
    -> result<void> {
    for (const auto& commitment : value.disclosure_commitments) {
        if (commitment.value.size() != 32) {
            return std::unexpected(invalid("attribute commitment must be 32 bytes"));
        }
        const auto found =
            std::ranges::find(profile.schema, commitment.name, &attestation_profile_field::name);
        if (found == profile.schema.end() || !found->may_commit) {
            return std::unexpected(invalid("attribute may not be committed: " + commitment.name));
        }
    }
    for (std::size_t index = 0; index < value.disclosure_commitments.size(); ++index) {
        for (std::size_t other = index + 1; other < value.disclosure_commitments.size(); ++other) {
            if (value.disclosure_commitments[index].name ==
                value.disclosure_commitments[other].name) {
                return std::unexpected(invalid("duplicate attribute commitment: " +
                                               value.disclosure_commitments[index].name));
            }
        }
    }
    return {};
}

auto has_commitment(const attestation& value, std::string_view name) -> bool {
    return std::ranges::any_of(value.disclosure_commitments,
                               [name](const auto& commitment) { return commitment.name == name; });
}

auto valid_resource(const slsa_resource_descriptor& value) -> bool {
    return !value.uri.empty() && !value.digest.empty() &&
           std::ranges::all_of(value.digest, [](const auto& entry) {
               return !entry.first.empty() && !entry.second.empty();
           });
}

auto validate_slsa_provenance(const attestation& value) -> result<void> {
    if (!value.attributes.empty() || !value.disclosure_commitments.empty()) {
        return std::unexpected(invalid("slsa_provenance/1 uses its structured predicate"));
    }
    if (!value.slsa_provenance) {
        return std::unexpected(invalid("SLSA provenance predicate is missing"));
    }
    const auto& predicate = *value.slsa_provenance;
    const auto& definition = predicate.build_definition;
    const auto& run = predicate.run_details;
    if (definition.build_type.empty() || definition.external_parameters.command.empty() ||
        !valid_resource(definition.internal_parameters.toolchain) ||
        !valid_resource(definition.internal_parameters.sandbox) ||
        definition.internal_parameters.platform.empty() || run.builder.id.empty() ||
        run.metadata.invocation_id.empty() || run.metadata.started_on.empty() ||
        run.metadata.finished_on.empty()) {
        return std::unexpected(invalid("SLSA provenance predicate is incomplete"));
    }
    if (!std::ranges::all_of(definition.resolved_dependencies, valid_resource) ||
        !std::ranges::all_of(run.byproducts, valid_resource)) {
        return std::unexpected(invalid("SLSA resource descriptor is incomplete"));
    }
    return {};
}

} // namespace

attestation_profile_registry::attestation_profile_registry()
    : profiles_{profile("org_membership/1",
                        {field("organization", attestation_field_type::text, true),
                         field("role", attestation_field_type::text, true),
                         field("display_name", attestation_field_type::text, false)},
                        {attestation_subject_kind::actor},
                        attestation_status_kind::revocable,
                        std::chrono::hours{24}),
                profile("delegation/1",
                        {field("principal", attestation_field_type::text, true),
                         field("scopes", attestation_field_type::text_list, true),
                         field("not_after", attestation_field_type::unsigned_integer, false)},
                        {attestation_subject_kind::actor},
                        attestation_status_kind::revocable,
                        std::chrono::hours{1}),
                profile("authorship/1",
                        {field("author", attestation_field_type::text, true)},
                        {attestation_subject_kind::drip},
                        attestation_status_kind::none),
                profile("review_approval/1",
                        {field("reviewer", attestation_field_type::text, true),
                         field("approved", attestation_field_type::boolean, true),
                         field("comment", attestation_field_type::text, false, true)},
                        {attestation_subject_kind::change},
                        attestation_status_kind::none),
                profile("slsa_provenance/1",
                        {},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("sbom/1",
                        {field("format", attestation_field_type::text, true),
                         field("output", attestation_field_type::text, true),
                         field("document", attestation_field_type::text, true),
                         field("derived", attestation_field_type::boolean, true),
                         field("inferred", attestation_field_type::boolean, true),
                         field("components", attestation_field_type::text_list, true, true),
                         field("supplier", attestation_field_type::text, false, true)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("delta/1",
                        {field("source", attestation_field_type::text, true),
                         field("target", attestation_field_type::text, true),
                         field("algorithm", attestation_field_type::text, true),
                         field("delta", attestation_field_type::text, true)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("model_provenance/1",
                        {field("training_dataset", attestation_field_type::text, true, true),
                         field("training_code", attestation_field_type::text, true, true),
                         field("base_model", attestation_field_type::text, false, true)},
                        {attestation_subject_kind::model},
                        attestation_status_kind::none),
                profile("abi_compatibility/1",
                        {field("compatible_with", attestation_field_type::text, true),
                         field("compatibility", attestation_field_type::text, true),
                         field("notes", attestation_field_type::text, false)},
                        {attestation_subject_kind::library},
                        attestation_status_kind::none),
                profile("scan_result/1",
                        {field("detectors", attestation_field_type::text_list, true),
                         field("findings", attestation_field_type::unsigned_integer, true),
                         field("allowances", attestation_field_type::text_list, true)},
                        {attestation_subject_kind::drip},
                        attestation_status_kind::max_age,
                        std::chrono::hours{24}),
                profile("reproducibility/1",
                        {field("verified_by", attestation_field_type::text, true),
                         field("sandbox", attestation_field_type::text, true),
                         field("agreement", attestation_field_type::boolean, true),
                         field("differing_outputs", attestation_field_type::text_list, false)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none)} {}

const attestation_profile_registry& attestation_profile_registry::global() {
    static const auto registry = attestation_profile_registry{};
    return registry;
}

const std::vector<attestation_profile>& attestation_profile_registry::profiles() const noexcept {
    return profiles_;
}

const attestation_profile*
attestation_profile_registry::find(std::string_view name) const noexcept {
    const auto found = std::ranges::find(profiles_, name, &attestation_profile::name);
    return found == profiles_.end() ? nullptr : &*found;
}

result<void> attestation_profile_registry::validate(const attestation& value) const {
    const auto* registered = find(value.profile);
    if (!registered) {
        return std::unexpected(invalid("unknown attestation profile: " + value.profile));
    }
    if (!std::ranges::contains(registered->subject_kinds, value.subject_kind)) {
        return std::unexpected(invalid("profile does not accept this subject kind"));
    }
    if (value.profile == "scan_result/1") {
        if (!value.attributes.empty() || !value.disclosure_commitments.empty() ||
            value.slsa_provenance) {
            return std::unexpected(invalid("scan_result/1 uses its fixed record fields"));
        }
        if (value.detectors.empty()) {
            return std::unexpected(invalid("required attribute is missing: detectors"));
        }
        for (const auto& allowance : value.allowances) {
            if (allowance.path.empty() || allowance.detector.empty() || allowance.reason.empty()) {
                return std::unexpected(invalid("scan allowance is incomplete"));
            }
        }
        return {};
    }
    if (value.profile == "slsa_provenance/1") {
        return validate_slsa_provenance(value);
    }
    if (value.slsa_provenance) {
        return std::unexpected(invalid("structured SLSA predicate belongs to slsa_provenance/1"));
    }
    for (std::size_t index = 0; index < value.attributes.size(); ++index) {
        const auto& attribute = value.attributes[index];
        for (std::size_t other = index + 1; other < value.attributes.size(); ++other) {
            if (attribute.name == value.attributes[other].name) {
                return std::unexpected(
                    invalid("duplicate attestation attribute: " + attribute.name));
            }
        }
        const auto found =
            std::ranges::find(registered->schema, attribute.name, &attestation_profile_field::name);
        if (found == registered->schema.end()) {
            return std::unexpected(invalid("unknown attribute for profile: " + attribute.name));
        }
        if (!is_type(attribute.value, found->type)) {
            return std::unexpected(invalid("attribute has the wrong type: " + attribute.name));
        }
    }
    auto commitments = validate_commitments(value, *registered);
    if (!commitments) {
        return std::unexpected(commitments.error());
    }
    for (const auto& field : registered->schema) {
        const auto present = std::ranges::any_of(value.attributes, [&field](const auto& attribute) {
            return attribute.name == field.name;
        });
        if (field.required && !present && !has_commitment(value, field.name)) {
            return std::unexpected(invalid("required attribute is missing: " + field.name));
        }
    }
    return {};
}

std::string_view to_string(attestation_subject_kind value) noexcept {
    switch (value) {
    case attestation_subject_kind::actor:
        return "actor";
    case attestation_subject_kind::drip:
        return "drip";
    case attestation_subject_kind::change:
        return "change";
    case attestation_subject_kind::artifact:
        return "artifact";
    case attestation_subject_kind::model:
        return "model";
    case attestation_subject_kind::library:
        return "library";
    }
    return "unknown";
}

std::string_view to_string(attestation_field_type value) noexcept {
    switch (value) {
    case attestation_field_type::text:
        return "text";
    case attestation_field_type::unsigned_integer:
        return "unsigned_integer";
    case attestation_field_type::boolean:
        return "boolean";
    case attestation_field_type::text_list:
        return "text_list";
    }
    return "unknown";
}

std::string_view to_string(attestation_status_kind value) noexcept {
    switch (value) {
    case attestation_status_kind::none:
        return "none";
    case attestation_status_kind::max_age:
        return "max_age";
    case attestation_status_kind::revocable:
        return "revocable";
    }
    return "unknown";
}

result<std::vector<std::byte>> attribute_commitment(const attestation_attribute& attribute,
                                                    std::span<const std::byte> salt) {
    auto encoded = cbor::encode(attribute_value(attribute.value));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto input = std::vector<std::byte>{};
    constexpr std::string_view domain = "TAAP/1 attribute";
    const auto append = [&input](std::string_view text) {
        input.insert(input.end(),
                     reinterpret_cast<const std::byte*>(text.data()),
                     reinterpret_cast<const std::byte*>(text.data() + text.size()));
    };
    append(domain);
    input.push_back(std::byte{});
    append(attribute.name);
    input.push_back(std::byte{});
    input.insert(input.end(), encoded->begin(), encoded->end());
    input.push_back(std::byte{});
    input.insert(input.end(), salt.begin(), salt.end());
    const auto digest = identity_detail::sha512_256(input);
    return std::vector<std::byte>{digest.begin(), digest.end()};
}

result<std::vector<std::byte>> encode_attestation_signature(const signature& value) {
    return identity_detail::canonical_signature(value);
}

result<attestation_verification>
verify_attestation(const attestation& value, const attestation_verification_options& options) {
    auto signed_value = identity_detail::parse_signature(value.signature);
    if (!signed_value) {
        return attestation_verification{false, signed_value.error().message};
    }
    auto body = objects::encode_attestation_body_unchecked(value);
    if (!body) {
        return attestation_verification{false, body.error().message};
    }
    auto signature = verify(value.issuer, "attestation", *body, *signed_value);
    if (!signature || !*signature) {
        return attestation_verification{
            false, signature ? "invalid attestation signature" : signature.error().message};
    }
    auto schema = attestation_profile_registry::global().validate(value);
    if (!schema) {
        return attestation_verification{false, schema.error().message};
    }
    const auto* profile = attestation_profile_registry::global().find(value.profile);
    const auto now =
        options.now == std::chrono::sys_seconds{}
            ? std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
            : options.now;
    if (profile->status_policy.kind == attestation_status_kind::max_age &&
        (value.at > now || now - value.at > profile->status_policy.maximum_age)) {
        return attestation_verification{false, "attestation is stale"};
    }
    if (profile->status_policy.kind == attestation_status_kind::revocable) {
        if (!options.status) {
            return attestation_verification{false, "attestation status is required"};
        }
        if (options.status->revoked) {
            return attestation_verification{false, "attestation is revoked"};
        }
        if (options.status->checked_at > now ||
            now - options.status->checked_at > profile->status_policy.maximum_age) {
            return attestation_verification{false, "attestation status is stale"};
        }
    }
    for (const auto& disclosure : options.disclosures) {
        const auto commitment = std::ranges::find(value.disclosure_commitments,
                                                  disclosure.attribute.name,
                                                  &attribute_disclosure_commitment::name);
        if (commitment == value.disclosure_commitments.end()) {
            return attestation_verification{false, "disclosed attribute has no commitment"};
        }
        auto expected = attribute_commitment(disclosure.attribute, disclosure.salt);
        if (!expected || !std::ranges::equal(*expected, commitment->value)) {
            return attestation_verification{false, "attribute commitment mismatch"};
        }
    }
    return attestation_verification{true, {}};
}

result<attestation> decode_attestation(std::span<const std::byte> bytes) {
    return objects::decode_attestation(bytes);
}

result<std::vector<std::byte>> encode_attestation(const attestation& value) {
    return objects::encode(value);
}

result<bool> verify(const attestation& value) {
    auto verified = verify_attestation(value);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    return verified->verified;
}

} // namespace liquid
