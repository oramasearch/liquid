// SPDX-License-Identifier: MIT

#include "liquid/attestation.hpp"

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
                        {field("scopes", attestation_field_type::text_list, true),
                         field("may_publish", attestation_field_type::boolean, true),
                         field("may_lease", attestation_field_type::boolean, true),
                         field("depth_remaining", attestation_field_type::unsigned_integer, true),
                         field("model", attestation_field_type::text, false),
                         field("dispatcher", attestation_field_type::text, true),
                         field("not_after", attestation_field_type::unsigned_integer, true),
                         field("repository", attestation_field_type::text, true)},
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
                profile("release_policy/1",
                        {field("role", attestation_field_type::text, true),
                         field("threshold", attestation_field_type::unsigned_integer, true),
                         field("signers", attestation_field_type::text_list, true),
                         field("not_after", attestation_field_type::text, true)},
                        {attestation_subject_kind::repository},
                        attestation_status_kind::none),
                profile("release_approval/1",
                        {field("policy", attestation_field_type::text, true),
                         field("release", attestation_field_type::text, true)},
                        {attestation_subject_kind::artifact},
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
                profile("projection/1",
                        {field("target", attestation_field_type::text, true),
                         field("source_result", attestation_field_type::text_list, true),
                         field("base_tree", attestation_field_type::text, false, true),
                         field("overlay_tree", attestation_field_type::text_list, false, true),
                         field("normalisation", attestation_field_type::text, true),
                         field("tool", attestation_field_type::text, true),
                         field("scripts", attestation_field_type::text_list, false)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("platform_signature/1",
                        {field("platform", attestation_field_type::text, true),
                         field("unsigned_digest", attestation_field_type::text, true),
                         field("signed_digest", attestation_field_type::text, true),
                         field("signing_identity", attestation_field_type::text, true),
                         field("verification", attestation_field_type::text, true),
                         field("notarization_ticket", attestation_field_type::text, false),
                         field("timestamp_response", attestation_field_type::text, false)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("delta/1",
                        {field("source", attestation_field_type::text, true),
                         field("target", attestation_field_type::text, true),
                         field("algorithm", attestation_field_type::text, true),
                         field("delta", attestation_field_type::text, true)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("hot_update/1",
                        {field("supersedes", attestation_field_type::text, true),
                         field("target", attestation_field_type::text, true),
                         field("tier", attestation_field_type::text, true),
                         field("symbols", attestation_field_type::text_list, true),
                         field("object", attestation_field_type::text, true),
                         field("patch", attestation_field_type::text, true),
                         field("outcome", attestation_field_type::text, true)},
                        {attestation_subject_kind::artifact},
                        attestation_status_kind::none),
                profile("image_provenance/1",
                        {field("image", attestation_field_type::text, true),
                         field("tree", attestation_field_type::text, true),
                         field("result", attestation_field_type::text, false)},
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
    case attestation_subject_kind::repository:
        return "repository";
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
    auto signature =
        options.issuer_chain.empty()
            ? verify(value.issuer, "attestation", *body, *signed_value)
            : verify(value.issuer, options.issuer_chain, "attestation", *body, *signed_value);
    if (!signature || !*signature) {
        return attestation_verification{false,
                                        signature ? "invalid attestation signature"
                                                  : signature.error().message,
                                        !signature && is_unknown_signer(signature.error())};
    }
    auto schema = attestation_profile_registry::global().validate(value);
    if (!schema) {
        return attestation_verification{false, schema.error().message};
    }
    const auto* profile = attestation_profile_registry::global().find(value.profile);
    // The clock never runs behind the newest signed time this process has accepted (L-35,
    // liquid#417): a clock rolled back cannot revive an attestation that has already expired. A
    // caller-supplied `now` (tests, a replayed evaluation) is used as given.
    static auto clock_floor = std::atomic<std::int64_t>{0};
    const auto system_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto now =
        options.now == std::chrono::sys_seconds{}
            ? std::max(system_now,
                       std::chrono::sys_seconds{std::chrono::seconds{clock_floor.load()}})
            : options.now;
    if (profile->status_policy.kind == attestation_status_kind::max_age &&
        (value.at > now || now - value.at > profile->status_policy.maximum_age)) {
        return attestation_verification{false, "attestation is stale"};
    }
    // Only a time already past on this clock raises the floor: a future-dated attestation must not
    // move it ahead and make every current one look stale.
    if (options.now == std::chrono::sys_seconds{} && value.at <= system_now) {
        const auto seen = static_cast<std::int64_t>(value.at.time_since_epoch().count());
        for (auto floor = clock_floor.load(); seen > floor && !clock_floor.compare_exchange_weak(floor, seen);) {
        }
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

result<signature_check> check_attestation_signature(const attestation& value) {
    auto signed_value = identity_detail::parse_signature(value.signature);
    if (!signed_value) {
        return std::unexpected{signed_value.error()};
    }
    auto body = objects::encode_attestation_body_unchecked(value);
    if (!body) {
        return std::unexpected{body.error()};
    }
    return check_signature(value.issuer, "attestation", *body, *signed_value);
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

result<void> validate_delegation(const attestation& value, const attestation* parent) {
    if (value.profile != "delegation/1") {
        return std::unexpected(invalid("attestation is not delegation/1"));
    }
    auto valid = attestation_profile_registry::global().validate(value);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (!parent) {
        return {};
    }
    if (parent->profile != "delegation/1") {
        return std::unexpected(invalid("delegation parent is not delegation/1"));
    }
    valid = attestation_profile_registry::global().validate(*parent);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    const auto attribute = [](const attestation& source,
                              std::string_view name) -> const attestation_value* {
        const auto found = std::ranges::find(source.attributes, name, &attestation_attribute::name);
        return found == source.attributes.end() ? nullptr : &found->value;
    };
    const auto* child_scopes = std::get_if<std::vector<std::string>>(attribute(value, "scopes"));
    const auto* parent_scopes = std::get_if<std::vector<std::string>>(attribute(*parent, "scopes"));
    const auto* child_publish = std::get_if<bool>(attribute(value, "may_publish"));
    const auto* parent_publish = std::get_if<bool>(attribute(*parent, "may_publish"));
    const auto* child_lease = std::get_if<bool>(attribute(value, "may_lease"));
    const auto* parent_lease = std::get_if<bool>(attribute(*parent, "may_lease"));
    const auto* child_depth = std::get_if<std::uint64_t>(attribute(value, "depth_remaining"));
    const auto* parent_depth = std::get_if<std::uint64_t>(attribute(*parent, "depth_remaining"));
    if (!child_scopes || !parent_scopes || !child_publish || !parent_publish || !child_lease ||
        !parent_lease || !child_depth || !parent_depth) {
        return std::unexpected(invalid("delegation attributes are incomplete"));
    }
    // Containment over the glob language sessions match (`*` and `?` never cross `/`, `**` does),
    // decided conservatively (liquid#517): a child scope is admitted only when it equals a parent
    // scope, the parent scope is `**`, or the parent scope is `P/**` and the child starts with
    // `P/`. Every other pair is refused, even one that might be contained, because a prefix of a
    // pattern with `*` or `?` says nothing about the paths it matches.
    const auto within = [](std::string_view child, std::string_view ancestor) {
        if (child == ancestor || ancestor == "**") {
            return true;
        }
        constexpr auto recursive = std::string_view{"/**"};
        if (!ancestor.ends_with(recursive)) {
            return false;
        }
        const auto directory = ancestor.substr(0, ancestor.size() - recursive.size() + 1);
        return directory.size() > 1 && child.starts_with(directory);
    };
    for (const auto& child : *child_scopes) {
        if (std::ranges::none_of(*parent_scopes,
                                 [&](const auto& ancestor) { return within(child, ancestor); })) {
            return std::unexpected(invalid("sub-delegation broadens path scopes"));
        }
    }
    // The chain is linked (liquid#517): the child is issued by the agent the parent delegates to,
    // for the same repository, no earlier than the parent and ending no later than it.
    auto issuer_subject = delegation_subject(value.issuer);
    if (!issuer_subject || issuer_subject->alg != parent->subject.alg ||
        issuer_subject->bytes != parent->subject.bytes) {
        return std::unexpected(invalid("sub-delegation is not issued by its parent's subject"));
    }
    const auto* child_repository = std::get_if<std::string>(attribute(value, "repository"));
    const auto* parent_repository = std::get_if<std::string>(attribute(*parent, "repository"));
    if (!child_repository || !parent_repository || *child_repository != *parent_repository) {
        return std::unexpected(invalid("sub-delegation names another repository"));
    }
    const auto* child_end = std::get_if<std::uint64_t>(attribute(value, "not_after"));
    const auto* parent_end = std::get_if<std::uint64_t>(attribute(*parent, "not_after"));
    if (!child_end || !parent_end || value.at < parent->at || *child_end > *parent_end) {
        return std::unexpected(invalid("sub-delegation is not within its parent's validity"));
    }
    if ((*child_publish && !*parent_publish) || (*child_lease && !*parent_lease)) {
        return std::unexpected(invalid("sub-delegation broadens authority"));
    }
    if (*parent_depth == 0 || *child_depth >= *parent_depth) {
        return std::unexpected(invalid("sub-delegation does not reduce depth_remaining"));
    }
    return {};
}

result<digest> delegation_subject(const actor& agent) {
    if (agent.taap_id.empty()) {
        return std::unexpected(invalid("delegation agent has no TAAP id"));
    }
    return hash(
        algorithm::blake3,
        std::span{reinterpret_cast<const std::byte*>(agent.taap_id.data()), agent.taap_id.size()});
}

result<void> check_delegation_window(const attestation& value, std::chrono::sys_seconds now) {
    const auto found = std::ranges::find(
        value.attributes, std::string_view{"not_after"}, &attestation_attribute::name);
    const auto* seconds =
        found == value.attributes.end() ? nullptr : std::get_if<std::uint64_t>(&found->value);
    if (!seconds) {
        return std::unexpected(invalid("delegation has no not_after"));
    }
    constexpr auto latest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (*seconds > latest) {
        return std::unexpected(invalid("delegation not_after is out of range"));
    }
    const auto not_after =
        std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(*seconds)}};
    if (not_after <= value.at) {
        return std::unexpected(invalid("delegation not_after is not after its issue time"));
    }
    if (not_after - value.at > delegation_maximum_lifetime) {
        return std::unexpected(invalid("delegation lifetime exceeds the maximum"));
    }
    if (value.at > now + delegation_clock_skew) {
        return std::unexpected(
            error{errc::unauthorized, "delegation is issued in the future", "delegation/1"});
    }
    if (now >= not_after) {
        return std::unexpected(error{errc::unauthorized, "delegation has expired", "delegation/1"});
    }
    return {};
}

delegation_status_resolver local_delegation_status() {
    return [](const attestation&, const digest& id) -> result<attestation_status> {
        const auto path = trust_file_path("delegation-revocations");
        const auto checked_at =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto code = std::error_code{};
        if (!std::filesystem::exists(path, code)) {
            if (code) {
                return std::unexpected(
                    error{errc::io, "cannot read delegation revocations", path.string()});
            }
            return attestation_status{.revoked = false, .checked_at = checked_at};
        }
        auto input = std::ifstream{path};
        if (!input) {
            return std::unexpected(
                error{errc::io, "cannot read delegation revocations", path.string()});
        }
        auto revoked = false;
        auto line = std::string{};
        while (std::getline(input, line)) {
            auto entry = std::string_view{line};
            if (const auto comment = entry.find('#'); comment != std::string_view::npos) {
                entry = entry.substr(0, comment);
            }
            constexpr auto blank = std::string_view{" \t\r\n"};
            const auto first = entry.find_first_not_of(blank);
            if (first == std::string_view::npos) {
                continue;
            }
            entry = entry.substr(first, entry.find_last_not_of(blank) - first + 1);
            auto listed = parse_digest(entry);
            if (!listed) {
                return std::unexpected(error{errc::invalid_argument,
                                             "delegation revocation is not a digest",
                                             path.string()});
            }
            revoked = revoked || (listed->alg == id.alg && listed->bytes == id.bytes);
        }
        if (input.bad()) {
            return std::unexpected(
                error{errc::io, "cannot read delegation revocations", path.string()});
        }
        return attestation_status{.revoked = revoked, .checked_at = checked_at};
    };
}

} // namespace liquid
