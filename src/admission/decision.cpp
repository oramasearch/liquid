#include "liquid/admission.hpp"

#include <charconv>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#include "encoding/cbor.hpp"
#include "formats/json.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;
namespace json = formats::json;

auto decision_error(std::string message, std::string context = "admission decision") -> error {
    return {errc::corrupt, std::move(message), std::move(context)};
}

auto cbor_map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto cbor_field(const cbor::value& value, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(*fields, [&](const auto& entry) {
        const auto* key = std::get_if<std::string>(&entry.first.data);
        return key && *key == name;
    });
    return found == fields->end() ? nullptr : &found->second;
}

auto json_field(const json::value& value, std::string_view name) -> const json::value* {
    const auto* fields = std::get_if<json::value::object>(&value.data);
    if (!fields) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *fields, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == fields->end() ? nullptr : &found->second;
}

auto effective_reasons(const admission_decision& value) -> std::vector<admission_reason> {
    if (!value.reasons.empty()) {
        return value.reasons;
    }
    if (!value.reason.empty()) {
        return {{.rule_id = "admission.reason", .message = value.reason}};
    }
    return {};
}

auto cbor_reasons(const admission_decision& value) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& reason : effective_reasons(value)) {
        output.push_back(cbor_map(
            {{"rule_id", cbor::value{reason.rule_id}}, {"message", cbor::value{reason.message}}}));
    }
    return cbor::value{std::move(output)};
}

auto cbor_evidence(const admission_decision& value) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& evidence : value.considered) {
        output.push_back(cbor_map({
            {"attestation", cbor::value{to_string(evidence.attestation)}},
            {"profile", cbor::value{evidence.profile}},
            {"issuer", cbor::value{evidence.issuer}},
            {"verified", cbor::value{evidence.verified}},
            {"rule_id", cbor::value{evidence.rule_id}},
            {"reason", cbor::value{evidence.reason}},
        }));
    }
    return cbor::value{std::move(output)};
}

auto json_reasons(const admission_decision& value) -> json::value {
    auto output = json::value::array{};
    for (const auto& reason : effective_reasons(value)) {
        output.emplace_back(json::value::object{
            {"rule_id", json::value{reason.rule_id}},
            {"message", json::value{reason.message}},
        });
    }
    return json::value{std::move(output)};
}

auto json_evidence(const admission_decision& value) -> json::value {
    auto output = json::value::array{};
    for (const auto& evidence : value.considered) {
        output.emplace_back(json::value::object{
            {"attestation", json::value{to_string(evidence.attestation)}},
            {"profile", json::value{evidence.profile}},
            {"issuer", json::value{evidence.issuer}},
            {"verified", json::value{evidence.verified}},
            {"rule_id", json::value{evidence.rule_id}},
            {"reason", json::value{evidence.reason}},
        });
    }
    return json::value{std::move(output)};
}

auto decision_json_value(const admission_decision& value) -> json::value {
    return json::value{json::value::object{
        {"v", json::value{json::number{std::to_string(value.v)}}},
        {"kind", json::value{std::string{"admission-decision"}}},
        {"sequence", json::value{json::number{std::to_string(value.sequence)}}},
        {"policy", json::value{value.policy}},
        {"subject",
         value.subject ? json::value{to_string(*value.subject)} : json::value{json::null{}}},
        {"image", json::value{value.image}},
        {"outcome", json::value{std::string{value.allowed ? "allowed" : "denied"}}},
        {"reasons", json_reasons(value)},
        {"evidence", json_evidence(value)},
    }};
}

auto parse_cbor_text(const cbor::value* value, std::string_view field) -> result<std::string> {
    const auto* text = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!text) {
        return std::unexpected(decision_error("field must be text", std::string{field}));
    }
    return *text;
}

auto parse_cbor_unsigned(const cbor::value* value, std::string_view field)
    -> result<std::uint64_t> {
    const auto* number = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
    if (!number) {
        return std::unexpected(
            decision_error("field must be an unsigned integer", std::string{field}));
    }
    return *number;
}

auto parse_json_text(const json::value* value, std::string_view field) -> result<std::string> {
    const auto* text = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!text) {
        return std::unexpected(decision_error("field must be text", std::string{field}));
    }
    return *text;
}

auto parse_json_unsigned(const json::value* value, std::string_view field)
    -> result<std::uint64_t> {
    const auto* number = value ? std::get_if<json::number>(&value->data) : nullptr;
    if (!number) {
        return std::unexpected(
            decision_error("field must be an unsigned integer", std::string{field}));
    }
    auto output = std::uint64_t{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        return std::unexpected(
            decision_error("field must be an unsigned integer", std::string{field}));
    }
    return output;
}

auto parse_subject(std::string_view value, std::string_view field) -> result<digest> {
    auto parsed = parse_digest(value);
    if (!parsed) {
        auto failure = parsed.error();
        failure.context = field;
        return std::unexpected(std::move(failure));
    }
    return *parsed;
}

auto decode_cbor_reasons(const cbor::value* value) -> result<std::vector<admission_reason>> {
    const auto* values = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!values) {
        return std::unexpected(decision_error("field must be an array", "reasons"));
    }
    auto output = std::vector<admission_reason>{};
    for (const auto& item : *values) {
        auto rule = parse_cbor_text(cbor_field(item, "rule_id"), "reasons.rule_id");
        auto message = parse_cbor_text(cbor_field(item, "message"), "reasons.message");
        if (!rule) {
            return std::unexpected(rule.error());
        }
        if (!message) {
            return std::unexpected(message.error());
        }
        output.push_back({.rule_id = std::move(*rule), .message = std::move(*message)});
    }
    return output;
}

auto decode_json_reasons(const json::value* value) -> result<std::vector<admission_reason>> {
    const auto* values = value ? std::get_if<json::value::array>(&value->data) : nullptr;
    if (!values) {
        return std::unexpected(decision_error("field must be an array", "reasons"));
    }
    auto output = std::vector<admission_reason>{};
    for (const auto& item : *values) {
        auto rule = parse_json_text(json_field(item, "rule_id"), "reasons.rule_id");
        auto message = parse_json_text(json_field(item, "message"), "reasons.message");
        if (!rule) {
            return std::unexpected(rule.error());
        }
        if (!message) {
            return std::unexpected(message.error());
        }
        output.push_back({.rule_id = std::move(*rule), .message = std::move(*message)});
    }
    return output;
}

auto decode_cbor_evidence(const cbor::value* value)
    -> result<std::vector<admission_attestation_verdict>> {
    const auto* values = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!values) {
        return std::unexpected(decision_error("field must be an array", "evidence"));
    }
    auto output = std::vector<admission_attestation_verdict>{};
    for (const auto& item : *values) {
        auto attestation = parse_cbor_text(cbor_field(item, "attestation"), "evidence.attestation");
        auto profile = parse_cbor_text(cbor_field(item, "profile"), "evidence.profile");
        auto issuer = parse_cbor_text(cbor_field(item, "issuer"), "evidence.issuer");
        const auto* verified_value = cbor_field(item, "verified");
        const auto* verified = verified_value ? std::get_if<bool>(&verified_value->data) : nullptr;
        auto rule = parse_cbor_text(cbor_field(item, "rule_id"), "evidence.rule_id");
        auto reason = parse_cbor_text(cbor_field(item, "reason"), "evidence.reason");
        if (!attestation) {
            return std::unexpected(attestation.error());
        }
        auto id = parse_subject(*attestation, "evidence.attestation");
        if (!id) {
            return std::unexpected(id.error());
        }
        if (!profile) {
            return std::unexpected(profile.error());
        }
        if (!issuer) {
            return std::unexpected(issuer.error());
        }
        if (!verified) {
            return std::unexpected(decision_error("field must be boolean", "evidence.verified"));
        }
        if (!rule) {
            return std::unexpected(rule.error());
        }
        if (!reason) {
            return std::unexpected(reason.error());
        }
        output.push_back({.attestation = *id,
                          .profile = std::move(*profile),
                          .issuer = std::move(*issuer),
                          .verified = *verified,
                          .rule_id = std::move(*rule),
                          .reason = std::move(*reason)});
    }
    return output;
}

auto decode_json_evidence(const json::value* value)
    -> result<std::vector<admission_attestation_verdict>> {
    const auto* values = value ? std::get_if<json::value::array>(&value->data) : nullptr;
    if (!values) {
        return std::unexpected(decision_error("field must be an array", "evidence"));
    }
    auto output = std::vector<admission_attestation_verdict>{};
    for (const auto& item : *values) {
        auto attestation = parse_json_text(json_field(item, "attestation"), "evidence.attestation");
        auto profile = parse_json_text(json_field(item, "profile"), "evidence.profile");
        auto issuer = parse_json_text(json_field(item, "issuer"), "evidence.issuer");
        const auto* verified_value = json_field(item, "verified");
        const auto* verified = verified_value ? std::get_if<bool>(&verified_value->data) : nullptr;
        auto rule = parse_json_text(json_field(item, "rule_id"), "evidence.rule_id");
        auto reason = parse_json_text(json_field(item, "reason"), "evidence.reason");
        if (!attestation) {
            return std::unexpected(attestation.error());
        }
        auto id = parse_subject(*attestation, "evidence.attestation");
        if (!id) {
            return std::unexpected(id.error());
        }
        if (!profile) {
            return std::unexpected(profile.error());
        }
        if (!issuer) {
            return std::unexpected(issuer.error());
        }
        if (!verified) {
            return std::unexpected(decision_error("field must be boolean", "evidence.verified"));
        }
        if (!rule) {
            return std::unexpected(rule.error());
        }
        if (!reason) {
            return std::unexpected(reason.error());
        }
        output.push_back({.attestation = *id,
                          .profile = std::move(*profile),
                          .issuer = std::move(*issuer),
                          .verified = *verified,
                          .rule_id = std::move(*rule),
                          .reason = std::move(*reason)});
    }
    return output;
}

} // namespace

result<std::vector<std::byte>> encode_admission_decision_cbor(const admission_decision& value) {
    return cbor::encode(cbor_map({
        {"v", cbor::value{value.v}},
        {"kind", cbor::value{"admission-decision"}},
        {"sequence", cbor::value{value.sequence}},
        {"policy", cbor::value{value.policy}},
        {"subject",
         value.subject ? cbor::value{to_string(*value.subject)} : cbor::value{cbor::null{}}},
        {"image", cbor::value{value.image}},
        {"outcome", cbor::value{value.allowed ? "allowed" : "denied"}},
        {"reasons", cbor_reasons(value)},
        {"evidence", cbor_evidence(value)},
    }));
}

result<admission_decision> decode_admission_decision_cbor(std::span<const std::byte> input) {
    auto document = cbor::decode(input);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto version = parse_cbor_unsigned(cbor_field(*document, "v"), "v");
    auto kind = parse_cbor_text(cbor_field(*document, "kind"), "kind");
    auto sequence = parse_cbor_unsigned(cbor_field(*document, "sequence"), "sequence");
    auto policy = parse_cbor_text(cbor_field(*document, "policy"), "policy");
    auto image = parse_cbor_text(cbor_field(*document, "image"), "image");
    auto outcome = parse_cbor_text(cbor_field(*document, "outcome"), "outcome");
    auto reasons = decode_cbor_reasons(cbor_field(*document, "reasons"));
    auto evidence = decode_cbor_evidence(cbor_field(*document, "evidence"));
    if (!version || !kind || !sequence || !policy || !image || !outcome || !reasons || !evidence) {
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!sequence) {
            return std::unexpected(sequence.error());
        }
        if (!policy) {
            return std::unexpected(policy.error());
        }
        if (!image) {
            return std::unexpected(image.error());
        }
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        if (!reasons) {
            return std::unexpected(reasons.error());
        }
        return std::unexpected(evidence.error());
    }
    if (*version != 1 || *kind != "admission-decision") {
        return std::unexpected(decision_error("unsupported admission decision record"));
    }
    if (*outcome != "allowed" && *outcome != "denied") {
        return std::unexpected(decision_error("unsupported decision outcome", "outcome"));
    }
    auto output = admission_decision{.v = *version,
                                     .sequence = *sequence,
                                     .policy = std::move(*policy),
                                     .allowed = *outcome == "allowed",
                                     .image = std::move(*image),
                                     .reasons = std::move(*reasons),
                                     .considered = std::move(*evidence)};
    const auto* subject = cbor_field(*document, "subject");
    if (!subject) {
        return std::unexpected(decision_error("field is required", "subject"));
    }
    if (const auto* text = std::get_if<std::string>(&subject->data)) {
        auto parsed = parse_subject(*text, "subject");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.subject = *parsed;
    } else if (!std::holds_alternative<cbor::null>(subject->data)) {
        return std::unexpected(decision_error("field must be a digest or null", "subject"));
    }
    if (!output.reasons.empty()) {
        output.reason = output.reasons.front().message;
    }
    const auto accepted =
        std::ranges::find_if(output.considered, [](const auto& item) { return item.verified; });
    if (accepted != output.considered.end()) {
        output.attestation = accepted->attestation;
    }
    return output;
}

result<std::string> encode_admission_decision_json(const admission_decision& value) {
    return json::write(decision_json_value(value));
}

result<admission_decision> decode_admission_decision_json(std::string_view input) {
    auto document = json::parse(input);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto version = parse_json_unsigned(json_field(*document, "v"), "v");
    auto kind = parse_json_text(json_field(*document, "kind"), "kind");
    auto sequence = parse_json_unsigned(json_field(*document, "sequence"), "sequence");
    auto policy = parse_json_text(json_field(*document, "policy"), "policy");
    auto image = parse_json_text(json_field(*document, "image"), "image");
    auto outcome = parse_json_text(json_field(*document, "outcome"), "outcome");
    auto reasons = decode_json_reasons(json_field(*document, "reasons"));
    auto evidence = decode_json_evidence(json_field(*document, "evidence"));
    if (!version || !kind || !sequence || !policy || !image || !outcome || !reasons || !evidence) {
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!sequence) {
            return std::unexpected(sequence.error());
        }
        if (!policy) {
            return std::unexpected(policy.error());
        }
        if (!image) {
            return std::unexpected(image.error());
        }
        if (!outcome) {
            return std::unexpected(outcome.error());
        }
        if (!reasons) {
            return std::unexpected(reasons.error());
        }
        return std::unexpected(evidence.error());
    }
    if (*version != 1 || *kind != "admission-decision") {
        return std::unexpected(decision_error("unsupported admission decision record"));
    }
    if (*outcome != "allowed" && *outcome != "denied") {
        return std::unexpected(decision_error("unsupported decision outcome", "outcome"));
    }
    auto output = admission_decision{.v = *version,
                                     .sequence = *sequence,
                                     .policy = std::move(*policy),
                                     .allowed = *outcome == "allowed",
                                     .image = std::move(*image),
                                     .reasons = std::move(*reasons),
                                     .considered = std::move(*evidence)};
    const auto* subject = json_field(*document, "subject");
    if (!subject) {
        return std::unexpected(decision_error("field is required", "subject"));
    }
    if (const auto* text = std::get_if<std::string>(&subject->data)) {
        auto parsed = parse_subject(*text, "subject");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.subject = *parsed;
    } else if (!std::holds_alternative<json::null>(subject->data)) {
        return std::unexpected(decision_error("field must be a digest or null", "subject"));
    }
    if (!output.reasons.empty()) {
        output.reason = output.reasons.front().message;
    }
    const auto accepted =
        std::ranges::find_if(output.considered, [](const auto& item) { return item.verified; });
    if (accepted != output.considered.end()) {
        output.attestation = accepted->attestation;
    }
    return output;
}

} // namespace liquid
