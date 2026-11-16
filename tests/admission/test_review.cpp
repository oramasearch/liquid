// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

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

    liquid::runtime::task<liquid::admission_log_verification>
    verify_inclusion(std::vector<std::byte> attestation) override {
        static_cast<void>(attestation);
        auto selected = verdict;
        if (calls < verdicts.size()) {
            selected = verdicts[calls++];
        }
        auto reason = std::string{"included in verified log"};
        if (selected == liquid::log_verdict::not_included) {
            reason = "not in log";
        } else if (selected == liquid::log_verdict::unavailable) {
            reason = "log verifier unavailable";
        } else if (selected == liquid::log_verdict::denied) {
            reason = "log verification denied by policy";
        }
        co_return liquid::admission_log_verification{.verdict = selected,
                                                     .reason = std::move(reason)};
    }
};

auto policy_for(const liquid::attestation& statement) -> liquid::admission_policy {
    return {.mode = liquid::admission_mode::attestation,
            .issuers = {statement.issuer.taap_id},
            .profiles = {"image_provenance/1"},
            .exempt = {"registry.example/exempt/*"},
            .exempt_namespaces = {"kube-system"},
            .attestation_source = "/store",
            .log = {.mode = liquid::admission_log_mode::verify,
                    .lake = "http://lake.example",
                    .trust = {.identity = {.taap_id = "taap:1:ed25519:fixture-log",
                                           .algorithm = "ed25519",
                                           .public_key = std::vector<std::byte>(32)},
                              .configuration =
                                  {
                                      .witnesses = {{
                                          .endpoint = "https://witness.example/checkpoint",
                                          .public_key = liquid::transparency::hash{std::byte{1}},
                                      }},
                                      .witness_threshold = 1,
                                  }},
                    .max_checkpoint_age_seconds = 300}};
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
    EXPECT_EQ(decoded->log.mode, liquid::admission_log_mode::verify);
    EXPECT_EQ(decoded->log.lake, policy.log.lake);
    EXPECT_EQ(decoded->log.trust.identity.taap_id, policy.log.trust.identity.taap_id);
    EXPECT_EQ(decoded->log.trust.identity.public_key, policy.log.trust.identity.public_key);
    EXPECT_EQ(decoded->log.trust.configuration.witness_threshold, 1U);
    ASSERT_EQ(decoded->log.trust.configuration.witnesses.size(), 1U);
    EXPECT_EQ(decoded->log.trust.configuration.witnesses.front().endpoint,
              "https://witness.example/checkpoint");
    EXPECT_EQ(decoded->log.trust.configuration.witnesses.front().public_key,
              policy.log.trust.configuration.witnesses.front().public_key);
    EXPECT_EQ(decoded->log.max_checkpoint_age_seconds, 300U);
}

TEST(AdmissionPolicy, refuses_an_invalid_verify_url) {
    const auto statement = signed_image();
    auto policy = policy_for(statement);
    policy.log.lake = "lake.example";
    auto encoded = liquid::encode_admission_policy(policy);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::decode_admission_policy(*encoded);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().context, "log.lake");
}

TEST(AdmissionPolicy, preserves_an_omitted_checkpoint_age_bound) {
    auto policy = policy_for(signed_image());
    policy.log.max_checkpoint_age_seconds.reset();
    const auto encoded = liquid::encode_admission_policy(policy);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded->find("max_checkpoint_age_seconds"), std::string::npos);
    const auto decoded = liquid::decode_admission_policy(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(decoded->log.max_checkpoint_age_seconds.has_value());
}

TEST(AdmissionPolicy, refuses_zero_and_malformed_checkpoint_age_bounds_at_load) {
    auto policy = policy_for(signed_image());
    policy.log.max_checkpoint_age_seconds = 987654321;
    const auto encoded = liquid::encode_admission_policy(policy);
    ASSERT_TRUE(encoded.has_value());
    for (const auto invalid : {"0", "-1", "null", "true", "1.5", "\"1\"", "18446744073709551616"}) {
        SCOPED_TRACE(invalid);
        auto input = *encoded;
        const auto offset = input.find("987654321");
        ASSERT_NE(offset, std::string::npos);
        input.replace(offset, 9, invalid);
        const auto decoded = liquid::decode_admission_policy(input);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error().code, liquid::errc::invalid_argument);
        EXPECT_EQ(decoded.error().context, "log.max_checkpoint_age_seconds");
    }
    policy.log.max_checkpoint_age_seconds = 0;
    EXPECT_FALSE(liquid::encode_admission_policy(policy).has_value());
}

TEST(AdmissionDecision, round_trips_cbor_and_json) {
    const auto value = liquid::admission_decision{
        .sequence = 42,
        .policy = "strict",
        .allowed = false,
        .image = image,
        .subject = image_subject(),
        .reason = "issuer not accepted",
        .reasons = {{.rule_id = "attestation.issuer_rejected", .message = "issuer not accepted"}},
        .considered = {{.attestation = image_subject(),
                        .profile = "image_provenance/1",
                        .issuer = "taap:1:ed25519:rejected",
                        .verified = false,
                        .rule_id = "attestation.issuer_rejected",
                        .reason = "issuer not accepted",
                        .log =
                            liquid::admission_log_verification{
                                .verdict = liquid::log_verdict::not_included,
                                .reason = "not in log"}}},
    };
    auto cbor = liquid::encode_admission_decision_cbor(value);
    ASSERT_TRUE(cbor) << cbor.error().message;
    auto decoded_cbor = liquid::decode_admission_decision_cbor(*cbor);
    ASSERT_TRUE(decoded_cbor) << decoded_cbor.error().message;
    auto reencoded_cbor = liquid::encode_admission_decision_cbor(*decoded_cbor);
    ASSERT_TRUE(reencoded_cbor) << reencoded_cbor.error().message;
    EXPECT_EQ(*reencoded_cbor, *cbor);

    auto json = liquid::encode_admission_decision_json(value);
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(json->find(R"("outcome": "not_included")"), std::string::npos);
    auto decoded_json = liquid::decode_admission_decision_json(*json);
    ASSERT_TRUE(decoded_json) << decoded_json.error().message;
    auto reencoded_json = liquid::encode_admission_decision_json(*decoded_json);
    ASSERT_TRUE(reencoded_json) << reencoded_json.error().message;
    EXPECT_EQ(*reencoded_json, *json);
}

TEST(AdmissionReview, applies_all_image_decisions_without_assumption) {
    auto statement = signed_image();
    auto policy = policy_for(statement);
    auto records = source{};
    records.values = {statement};
    auto verifier = log{};

    auto accepted = review(policy, records, verifier);
    EXPECT_TRUE(accepted.allowed);
    ASSERT_TRUE(accepted.subject.has_value());
    EXPECT_EQ(accepted.subject->alg, image_subject().alg);
    EXPECT_EQ(accepted.subject->bytes, image_subject().bytes);
    EXPECT_TRUE(accepted.attestation.has_value());
    ASSERT_EQ(accepted.reasons.size(), 1U);
    EXPECT_EQ(accepted.reasons.front().rule_id, "attestation.verified");
    ASSERT_EQ(accepted.considered.size(), 1U);
    EXPECT_TRUE(accepted.considered.front().verified);
    EXPECT_EQ(accepted.considered.front().rule_id, "attestation.verified");

    records.values.clear();
    auto absent = review(policy, records, verifier);
    EXPECT_FALSE(absent.allowed);
    EXPECT_EQ(absent.reason, "no attestation");
    ASSERT_EQ(absent.reasons.size(), 1U);
    EXPECT_EQ(absent.reasons.front().rule_id, "attestation.missing");

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
    const auto logged = review(policy, records, verifier);
    EXPECT_TRUE(logged.allowed);
    ASSERT_EQ(logged.considered.size(), 2U);
    ASSERT_TRUE(logged.considered.back().log.has_value());
    EXPECT_EQ(logged.considered.back().log->verdict, liquid::log_verdict::included);
    EXPECT_EQ(verifier.calls, 2U);

    records.values = {statement};
    verifier.verdicts.clear();
    verifier.verdict = liquid::log_verdict::not_included;
    EXPECT_EQ(review(policy, records, verifier).reason, "not in log");

    records.available = false;
    policy.mode = liquid::admission_mode::attestation;
    EXPECT_EQ(review(policy, records, verifier).reason, "attestation source unavailable");
}

// liquid#383: the policy names the issuer, but a server without the issuer's identity chain cannot
// bind that name to the signing key, so the image is refused as signed by an unknown identity.
TEST(AdmissionReview, refuses_an_issuer_whose_identity_chain_is_unknown) {
    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto issuer_home = std::filesystem::temp_directory_path() /
                             ("liquid-admission-issuer-" + std::to_string(::getpid()));
    const auto server_home = std::filesystem::temp_directory_path() /
                             ("liquid-admission-server-" + std::to_string(::getpid()));
    std::error_code code;
    std::filesystem::remove_all(issuer_home, code);
    std::filesystem::remove_all(server_home, code);
    setenv("LIQUID_HOME", issuer_home.c_str(), 1);
    auto statement = signed_image();
    auto policy = policy_for(statement);
    auto records = source{};
    records.values = {statement};
    auto verifier = log{};

    setenv("LIQUID_HOME", server_home.c_str(), 1);
    const auto refused = review(policy, records, verifier);
    EXPECT_FALSE(refused.allowed);
    EXPECT_EQ(refused.reason, "issuer identity unknown");
    ASSERT_EQ(refused.considered.size(), 1U);
    EXPECT_EQ(refused.considered.front().rule_id, "attestation.unknown_signer");

    setenv("LIQUID_HOME", issuer_home.c_str(), 1);
    auto chain = liquid::load_identity_chain(statement.issuer.taap_id);
    ASSERT_TRUE(chain) << chain.error().message;
    setenv("LIQUID_HOME", server_home.c_str(), 1);
    ASSERT_TRUE(liquid::trust_identity_chain(*chain));
    EXPECT_TRUE(review(policy, records, verifier).allowed);

    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    std::filesystem::remove_all(issuer_home, code);
    std::filesystem::remove_all(server_home, code);
}

} // namespace
