#include "liquid/admission.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace liquid {
namespace {

auto glob_matches(std::string_view pattern, std::string_view value) -> bool {
    auto previous = std::vector<bool>(value.size() + 1);
    previous[0] = true;
    for (const auto pattern_character : pattern) {
        auto current = std::vector<bool>(value.size() + 1);
        if (pattern_character == '*') {
            current[0] = previous[0];
            for (auto index = std::size_t{1}; index <= value.size(); ++index) {
                current[index] = previous[index] || current[index - 1];
            }
        } else {
            for (auto index = std::size_t{1}; index <= value.size(); ++index) {
                current[index] = previous[index - 1] && (pattern_character == '?' ||
                                                         pattern_character == value[index - 1]);
            }
        }
        previous = std::move(current);
    }
    return previous[value.size()];
}

auto image_digest(std::string_view reference) -> result<digest> {
    const auto separator = reference.rfind('@');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == reference.size()) {
        return std::unexpected(
            error{errc::invalid_argument, "image not pinned by digest", std::string{reference}});
    }
    auto parsed = parse_digest(reference.substr(separator + 1));
    if (!parsed || parsed->alg != algorithm::sha256) {
        return std::unexpected(
            error{errc::invalid_argument, "image not pinned by digest", std::string{reference}});
    }
    return *parsed;
}

auto record_digest(const attestation& value) -> result<digest> {
    auto encoded = encode_attestation(value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(algorithm::blake3, *encoded);
}

auto policy_contains(std::span<const std::string> values, std::string_view value) -> bool {
    return std::ranges::find(values, value) != values.end();
}

auto add_reason(admission_decision& decision,
                std::string rule_id,
                std::string message,
                bool summary = true) -> void {
    if (summary) {
        decision.reason = message;
    }
    decision.reasons.push_back(
        admission_reason{.rule_id = std::move(rule_id), .message = std::move(message)});
}

auto refuse(admission_decision& decision, std::string rule_id, std::string message) -> void {
    decision.allowed = false;
    add_reason(decision, std::move(rule_id), std::move(message));
}

auto evidence_reason(admission_attestation_verdict& evidence,
                     std::string rule_id,
                     std::string reason) -> void {
    evidence.rule_id = std::move(rule_id);
    evidence.reason = std::move(reason);
}

} // namespace

runtime::task<admission_decision> review_image(const admission_policy& policy,
                                               std::string image_reference,
                                               attestation_source& source,
                                               log_verifier& log) {
    auto output = admission_decision{.image = std::move(image_reference)};
    if (policy.mode == admission_mode::off) {
        output.allowed = true;
        output.reason = "exempt";
        add_reason(output, "policy.audit_only", "audit-only policy admitted image", false);
        co_return output;
    }
    if (std::ranges::any_of(policy.exempt, [&](const auto& pattern) {
            return glob_matches(pattern, output.image);
        })) {
        output.allowed = true;
        output.reason = "exempt";
        add_reason(output, "policy.image_exempt", "image matched an exemption", false);
        co_return output;
    }
    auto subject = image_digest(output.image);
    if (!subject) {
        refuse(output, "image.digest_required", "image not pinned by digest");
        co_return output;
    }
    output.subject = *subject;
    auto attestations = co_await source.by_subject(*subject);
    if (!attestations) {
        refuse(output, "attestation.source_unavailable", "attestation source unavailable");
        co_return output;
    }
    auto accepted_issuer = false;
    auto accepted_profile = false;
    auto invalid_signature = false;
    auto unknown_signer = false;
    auto log_refusal = std::optional<admission_reason>{};
    for (const auto& statement : *attestations) {
        auto id = record_digest(statement);
        if (!id) {
            invalid_signature = true;
            continue;
        }
        auto considered = admission_attestation_verdict{
            .attestation = *id, .profile = statement.profile, .issuer = statement.issuer.taap_id};
        if (statement.subject.alg != subject->alg || statement.subject.bytes != subject->bytes) {
            evidence_reason(considered, "attestation.subject_mismatch", "signature invalid");
            invalid_signature = true;
            output.considered.push_back(std::move(considered));
            continue;
        }
        if (!policy_contains(policy.issuers, statement.issuer.taap_id)) {
            evidence_reason(considered, "attestation.issuer_rejected", "issuer not accepted");
            output.considered.push_back(std::move(considered));
            continue;
        }
        accepted_issuer = true;
        if (!policy_contains(policy.profiles, statement.profile)) {
            evidence_reason(considered, "attestation.profile_rejected", "profile not accepted");
            output.considered.push_back(std::move(considered));
            continue;
        }
        accepted_profile = true;
        auto verified = verify_attestation(statement);
        if (verified && verified->unknown_signer) {
            // The issuer is named in the policy but this server holds no identity chain for it,
            // so the name is not bound to a key and the signature proves nothing (liquid#383).
            evidence_reason(considered, "attestation.unknown_signer", "issuer identity unknown");
            unknown_signer = true;
            output.considered.push_back(std::move(considered));
            continue;
        }
        if (!verified || !verified->verified) {
            evidence_reason(considered, "attestation.signature_invalid", "signature invalid");
            invalid_signature = true;
            output.considered.push_back(std::move(considered));
            continue;
        }
        if (policy.mode == admission_mode::attestation_and_log) {
            auto encoded = encode_attestation(statement);
            auto included =
                encoded ? co_await log.verify_inclusion(std::move(*encoded))
                        : result<admission_log_verification>{std::unexpected(encoded.error())};
            if (!included) {
                included = admission_log_verification{
                    .verdict = log_verdict::unavailable,
                    .reason = included.error().message,
                };
            }
            considered.log = *included;
            if (included->verdict != log_verdict::included) {
                auto rule_id = std::string{"transparency.not_included"};
                if (included->verdict == log_verdict::unavailable) {
                    rule_id = "transparency.verifier_unavailable";
                } else if (included->verdict == log_verdict::denied) {
                    rule_id = "transparency.policy_denied";
                }
                auto reason =
                    included->reason.empty() ? std::string{"not in log"} : included->reason;
                evidence_reason(considered, rule_id, reason);
                if (!log_refusal) {
                    log_refusal = admission_reason{.rule_id = std::move(rule_id),
                                                   .message = std::move(reason)};
                }
                output.considered.push_back(std::move(considered));
                continue;
            }
        }
        considered.verified = true;
        evidence_reason(considered, "attestation.verified", "attestation accepted");
        output.considered.push_back(std::move(considered));
        output.allowed = true;
        output.attestation = *id;
        add_reason(output, "attestation.verified", "attestation accepted", false);
        co_return output;
    }
    if (attestations->empty()) {
        refuse(output, "attestation.missing", "no attestation");
    } else if (log_refusal) {
        refuse(output, std::move(log_refusal->rule_id), std::move(log_refusal->message));
    } else if (invalid_signature && accepted_profile) {
        refuse(output, "attestation.signature_invalid", "signature invalid");
    } else if (unknown_signer) {
        refuse(output, "attestation.unknown_signer", "issuer identity unknown");
    } else if (accepted_issuer) {
        refuse(output, "attestation.profile_rejected", "profile not accepted");
    } else {
        refuse(output, "attestation.issuer_rejected", "issuer not accepted");
    }
    co_return output;
}

} // namespace liquid
