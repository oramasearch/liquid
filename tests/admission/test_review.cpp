#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "identity/taap.hpp"
#include "liquid/admission.hpp"
#include "objects/records.hpp"

namespace {

constexpr auto image = "registry.example/liquid@sha256:"
                       "1111111111111111111111111111111111111111111111111111111111111111";

auto image_subject() -> liquid::digest {
    auto parsed =
        liquid::parse_digest(std::string_view{image}.substr(std::string_view{image}.find('@') + 1));
    EXPECT_TRUE(parsed) << parsed.error().message;
    return *parsed;
}

auto signed_image() -> liquid::attestation {
    auto value = liquid::attestation{
        .subject = image_subject(),
        .profile = "image_provenance/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = liquid::attestation_subject_kind::artifact,
        .attributes = {{"image", image}, {"tree", "blake3:tree"}},
    };
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    value.issuer = signer->identity();
    auto body = liquid::objects::encode_attestation_body(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    value.signature = std::move(*signature);
    return value;
}

class source final : public liquid::attestation_source {
public:
    std::vector<liquid::attestation> values;
    bool available{true};

    liquid::runtime::task<std::vector<liquid::attestation>>
    by_subject(liquid::digest subject) override {
        static_cast<void>(subject);
        if (!available) {
            co_return std::unexpected(
                liquid::error{liquid::errc::unavailable, "source unavailable"});
        }
        co_return values;
    }
};

class log final : public liquid::log_verifier {
public:
    liquid::log_verdict verdict{liquid::log_verdict::included};
    std::vector<liquid::log_verdict> verdicts;
    std::size_t calls{};

    liquid::runtime::task<liquid::log_verdict>
    verify_inclusion(liquid::digest attestation) override {
        static_cast<void>(attestation);
        if (calls < verdicts.size()) {
            co_return verdicts[calls++];
        }
        co_return verdict;
    }
};

auto policy_for(const liquid::attestation& statement) -> liquid::admission_policy {
    return {.mode = liquid::admission_mode::attestation,
            .issuers = {statement.issuer.taap_id},
            .profiles = {"image_provenance/1"},
            .exempt = {"registry.example/exempt/*"},
            .exempt_namespaces = {"kube-system"},
            .attestation_source = "/store",
            .log = {.lake = "http://lake.example", .max_checkpoint_age_seconds = 300}};
}

auto review(const liquid::admission_policy& policy,
            source& records,
            log& verifier,
            std::string reference = image) -> liquid::admission_decision {
    auto decision = liquid::runtime::block_on(
        liquid::review_image(policy, std::move(reference), records, verifier));
    EXPECT_TRUE(decision) << decision.error().message;
    return *decision;
}

TEST(AdmissionPolicy, round_trips_the_versioned_contract) {
    const auto statement = signed_image();
    const auto policy = policy_for(statement);
    auto encoded = liquid::encode_admission_policy(policy);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::decode_admission_policy(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->v, 1U);
    EXPECT_EQ(decoded->mode, liquid::admission_mode::attestation);
    EXPECT_EQ(decoded->issuers, policy.issuers);
    EXPECT_EQ(decoded->profiles, policy.profiles);
    EXPECT_EQ(decoded->exempt, policy.exempt);
    EXPECT_EQ(decoded->exempt_namespaces, policy.exempt_namespaces);
    EXPECT_EQ(decoded->attestation_source, policy.attestation_source);
    EXPECT_EQ(decoded->log.lake, policy.log.lake);
    EXPECT_EQ(decoded->log.max_checkpoint_age_seconds, 300U);
}

TEST(AdmissionReview, applies_all_image_decisions_without_assumption) {
    auto statement = signed_image();
    auto policy = policy_for(statement);
    auto records = source{};
    records.values = {statement};
    auto verifier = log{};

    auto accepted = review(policy, records, verifier);
    EXPECT_TRUE(accepted.allowed);
    EXPECT_TRUE(accepted.attestation.has_value());
    ASSERT_EQ(accepted.considered.size(), 1U);
    EXPECT_TRUE(accepted.considered.front().verified);

    records.values.clear();
    auto absent = review(policy, records, verifier);
    EXPECT_FALSE(absent.allowed);
    EXPECT_EQ(absent.reason, "no attestation");

    records.values = {statement};
    auto wrong_issuer = policy;
    wrong_issuer.issuers = {"taap:1:ed25519:not-accepted"};
    EXPECT_EQ(review(wrong_issuer, records, verifier).reason, "issuer not accepted");

    auto wrong_profile = policy;
    wrong_profile.profiles = {"slsa_provenance/1"};
    EXPECT_EQ(review(wrong_profile, records, verifier).reason, "profile not accepted");

    records.values.front().signature.front() ^= std::byte{1};
    EXPECT_EQ(review(policy, records, verifier).reason, "signature invalid");

    records.values.clear();
    EXPECT_EQ(review(policy, records, verifier, "registry.example/liquid:latest").reason,
              "image not pinned by digest");
    auto exempt = review(policy, records, verifier, "registry.example/exempt/tool:latest");
    EXPECT_TRUE(exempt.allowed);
    EXPECT_EQ(exempt.reason, "exempt");

    auto off = policy;
    off.mode = liquid::admission_mode::off;
    auto disabled = review(off, records, verifier, "registry.example/liquid:latest");
    EXPECT_TRUE(disabled.allowed);
    EXPECT_EQ(disabled.reason, "exempt");

    records.values = {statement};
    policy.mode = liquid::admission_mode::attestation_and_log;
    verifier.verdict = liquid::log_verdict::unavailable;
    EXPECT_EQ(review(policy, records, verifier).reason, "log verifier unavailable");

    records.values = {statement, statement};
    verifier.verdicts = {liquid::log_verdict::unavailable, liquid::log_verdict::included};
    verifier.calls = 0;
    EXPECT_TRUE(review(policy, records, verifier).allowed);

    records.values = {statement};
    verifier.verdicts.clear();
    verifier.verdict = liquid::log_verdict::not_included;
    EXPECT_EQ(review(policy, records, verifier).reason, "not in log");

    records.available = false;
    policy.mode = liquid::admission_mode::attestation;
    EXPECT_EQ(review(policy, records, verifier).reason, "attestation source unavailable");
}

} // namespace
