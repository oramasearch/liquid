#include "liquid/admission.hpp"

#include <algorithm>
#include <ranges>
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

} // namespace

runtime::task<admission_decision> review_image(const admission_policy& policy,
                                               std::string image_reference,
                                               attestation_source& source,
                                               log_verifier& log) {
    auto output = admission_decision{.image = std::move(image_reference)};
    if (policy.mode == admission_mode::off ||
        std::ranges::any_of(policy.exempt, [&](const auto& pattern) {
            return glob_matches(pattern, output.image);
        })) {
        output.allowed = true;
        output.reason = "exempt";
        co_return output;
    }
    auto subject = image_digest(output.image);
    if (!subject) {
        output.reason = "image not pinned by digest";
        co_return output;
    }
    auto attestations = co_await source.by_subject(*subject);
    if (!attestations) {
        output.reason = "attestation source unavailable";
        co_return output;
    }
    auto accepted_issuer = false;
    auto accepted_profile = false;
    auto invalid_signature = false;
    auto inclusion_unavailable = false;
    auto not_included = false;
    for (const auto& statement : *attestations) {
        auto id = record_digest(statement);
        if (!id) {
            invalid_signature = true;
            continue;
        }
        auto considered = admission_attestation_verdict{
            .attestation = *id, .profile = statement.profile, .issuer = statement.issuer.taap_id};
        if (statement.subject.alg != subject->alg || statement.subject.bytes != subject->bytes) {
            considered.reason = "signature invalid";
            invalid_signature = true;
            output.considered.push_back(std::move(considered));
            continue;
        }
        if (!policy_contains(policy.issuers, statement.issuer.taap_id)) {
            considered.reason = "issuer not accepted";
            output.considered.push_back(std::move(considered));
            continue;
        }
        accepted_issuer = true;
        if (!policy_contains(policy.profiles, statement.profile)) {
            considered.reason = "profile not accepted";
            output.considered.push_back(std::move(considered));
            continue;
        }
        accepted_profile = true;
        auto verified = verify_attestation(statement);
        if (!verified || !verified->verified) {
            considered.reason = "signature invalid";
            invalid_signature = true;
            output.considered.push_back(std::move(considered));
            continue;
        }
        if (policy.mode == admission_mode::attestation_and_log) {
            auto included = co_await log.verify_inclusion(*id);
            if (!included || *included == log_verdict::unavailable) {
                considered.reason = "log verifier unavailable";
                inclusion_unavailable = true;
                output.considered.push_back(std::move(considered));
                continue;
            }
            if (*included == log_verdict::not_included) {
                considered.reason = "not in log";
                not_included = true;
                output.considered.push_back(std::move(considered));
                continue;
            }
        }
        considered.verified = true;
        output.considered.push_back(std::move(considered));
        output.allowed = true;
        output.attestation = *id;
        co_return output;
    }
    if (attestations->empty()) {
        output.reason = "no attestation";
    } else if (inclusion_unavailable) {
        output.reason = "log verifier unavailable";
    } else if (not_included) {
        output.reason = "not in log";
    } else if (invalid_signature && accepted_profile) {
        output.reason = "signature invalid";
    } else if (accepted_issuer) {
        output.reason = "profile not accepted";
    } else {
        output.reason = "issuer not accepted";
    }
    co_return output;
}

} // namespace liquid
