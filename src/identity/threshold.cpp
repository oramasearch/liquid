#include "liquid/release.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <set>
#include <utility>

#include "objects/records.hpp"

namespace liquid {
namespace {

auto failure(errc code, std::string message) -> error {
    return {code, std::move(message), "release approval"};
}

auto attribute(const attestation& value, std::string_view name) -> const attestation_value* {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    return found == value.attributes.end() ? nullptr : &found->value;
}

auto policy_digest(const release_policy& policy) -> result<digest> {
    auto encoded = encode_attestation(policy.statement);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(algorithm::blake3, *encoded);
}

auto parse_timestamp(std::string_view value) -> result<std::chrono::sys_seconds> {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return std::unexpected(failure(errc::invalid_argument, "invalid release expiry"));
    }
    const auto number = [&](std::size_t offset, std::size_t count) -> result<int> {
        auto output = 0;
        for (std::size_t index = offset; index < offset + count; ++index) {
            if (value[index] < '0' || value[index] > '9') {
                return std::unexpected(failure(errc::invalid_argument, "invalid release expiry"));
            }
            output = output * 10 + value[index] - '0';
        }
        return output;
    };
    auto year = number(0, 4);
    auto month = number(5, 2);
    auto day = number(8, 2);
    auto hour = number(11, 2);
    auto minute = number(14, 2);
    auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::unexpected(failure(errc::invalid_argument, "invalid release expiry"));
    }
    const auto date = std::chrono::year{*year} /
                      std::chrono::month{static_cast<unsigned>(*month)} /
                      std::chrono::day{static_cast<unsigned>(*day)};
    if (!date.ok() || *hour >= 24 || *minute >= 60 || *second >= 60) {
        return std::unexpected(failure(errc::invalid_argument, "invalid release expiry"));
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
           std::chrono::minutes{*minute} + std::chrono::seconds{*second};
}

auto sign_statement(attestation value, signer& signing_key) -> runtime::task<attestation> {
    value.issuer = signing_key.identity();
    auto body = objects::encode_attestation_body(value);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await signing_key.sign("attestation", std::move(*body));
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto encoded_signature = encode_attestation_signature(*signed_value);
    if (!encoded_signature) {
        co_return std::unexpected(encoded_signature.error());
    }
    value.signature = std::move(*encoded_signature);
    co_return value;
}

} // namespace

runtime::task<release_policy> make_release_policy(digest repository,
                                                  std::string role,
                                                  std::uint64_t threshold,
                                                  std::vector<std::string> signers,
                                                  std::string not_after,
                                                  signer& policy_signer) {
    std::ranges::sort(signers);
    signers.erase(std::unique(signers.begin(), signers.end()), signers.end());
    auto expiry = parse_timestamp(not_after);
    if (!expiry || role.empty() || threshold == 0 || threshold > signers.size() ||
        std::ranges::any_of(signers, &std::string::empty)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "invalid release policy threshold or signers"));
    }
    auto value = attestation{.subject = repository,
                             .profile = "release_policy/1",
                             .at = std::chrono::floor<std::chrono::seconds>(
                                 std::chrono::system_clock::now()),
                             .subject_kind = attestation_subject_kind::repository,
                             .attributes = {{"role", std::move(role)},
                                            {"threshold", threshold},
                                            {"signers", std::move(signers)},
                                            {"not_after", std::move(not_after)}}};
    auto signed_value = co_await sign_statement(std::move(value), policy_signer);
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    co_return release_policy{std::move(*signed_value)};
}

runtime::task<release_approval> approve_release(digest artifact,
                                                const release_policy& policy,
                                                std::string release,
                                                signer& approving_signer) {
    auto policy_id = policy_digest(policy);
    if (!policy_id) {
        co_return std::unexpected(policy_id.error());
    }
    if (release.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "release name must not be empty"));
    }
    auto value = attestation{.subject = artifact,
                             .profile = "release_approval/1",
                             .at = std::chrono::floor<std::chrono::seconds>(
                                 std::chrono::system_clock::now()),
                             .subject_kind = attestation_subject_kind::artifact,
                             .attributes = {{"policy", to_string(*policy_id)},
                                            {"release", std::move(release)}}};
    auto signed_value = co_await sign_statement(std::move(value), approving_signer);
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    co_return release_approval{std::move(*signed_value)};
}

result<release_verification> verify_release(digest artifact,
                                            const release_policy& policy,
                                            std::span<const release_approval> approvals,
                                            std::chrono::sys_seconds now) {
    auto policy_verified = verify_attestation(policy.statement, {.now = now});
    if (!policy_verified || !policy_verified->verified) {
        return std::unexpected(failure(
            errc::unauthorized,
            policy_verified ? policy_verified->reason : policy_verified.error().message));
    }
    if (policy.statement.profile != "release_policy/1" ||
        policy.statement.subject_kind != attestation_subject_kind::repository) {
        return std::unexpected(failure(errc::unauthorized, "invalid release policy profile"));
    }
    const auto* threshold = std::get_if<std::uint64_t>(attribute(policy.statement, "threshold"));
    const auto* signers =
        std::get_if<std::vector<std::string>>(attribute(policy.statement, "signers"));
    const auto* not_after = std::get_if<std::string>(attribute(policy.statement, "not_after"));
    if (!threshold || !signers || !not_after || *threshold == 0 || *threshold > signers->size()) {
        return std::unexpected(failure(errc::unauthorized, "invalid release policy attributes"));
    }
    auto expiry = parse_timestamp(*not_after);
    if (!expiry) {
        return std::unexpected(failure(errc::unauthorized, "invalid release policy expiry"));
    }
    const auto checked_at = now == std::chrono::sys_seconds{}
                                ? std::chrono::floor<std::chrono::seconds>(
                                      std::chrono::system_clock::now())
                                : now;
    auto output = release_verification{};
    if (checked_at > *expiry) {
        output.missing = *signers;
        return output;
    }
    auto policy_id = policy_digest(policy);
    if (!policy_id) {
        return std::unexpected(policy_id.error());
    }
    const auto expected_policy = to_string(*policy_id);
    auto accepted = std::set<std::string>{};
    for (const auto& approval : approvals) {
        const auto& statement = approval.statement;
        const auto* approval_policy =
            std::get_if<std::string>(attribute(statement, "policy"));
        const auto subject_matches = statement.subject.alg == artifact.alg &&
                                     statement.subject.bytes == artifact.bytes;
        if (statement.profile != "release_approval/1" || !subject_matches ||
            statement.subject_kind != attestation_subject_kind::artifact || !approval_policy ||
            *approval_policy != expected_policy || statement.at > *expiry ||
            !std::ranges::contains(*signers, statement.issuer.taap_id)) {
            continue;
        }
        auto verified = verify_attestation(statement, {.now = checked_at});
        if (verified && verified->verified) {
            accepted.insert(statement.issuer.taap_id);
        }
    }
    output.distinct_valid_signers.assign(accepted.begin(), accepted.end());
    for (const auto& signer : *signers) {
        if (!accepted.contains(signer)) {
            output.missing.push_back(signer);
        }
    }
    output.satisfied = output.distinct_valid_signers.size() >= *threshold;
    return output;
}

} // namespace liquid
