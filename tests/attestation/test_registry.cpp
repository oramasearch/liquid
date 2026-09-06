#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"

namespace {

auto subject(liquid::attestation_subject_kind kind) -> liquid::digest {
    auto value = liquid::digest{liquid::algorithm::blake3, {}};
    value.bytes[0] = std::byte{static_cast<unsigned char>(kind)};
    return value;
}

auto example(std::string_view profile) -> liquid::attestation {
    auto value = liquid::attestation{.subject = subject(liquid::attestation_subject_kind::artifact),
                                     .profile = std::string{profile},
                                     .at = std::chrono::sys_seconds{std::chrono::seconds{100}},
                                     .subject_kind = liquid::attestation_subject_kind::artifact};
    if (profile == "org_membership/1") {
        value.subject_kind = liquid::attestation_subject_kind::actor;
        value.attributes = {{"organization", "liquid"}, {"role", "maintainer"}};
    } else if (profile == "delegation/1") {
        value.subject_kind = liquid::attestation_subject_kind::actor;
        value.attributes = {{"principal", "taap:1:ed25519:principal"},
                            {"scopes", std::vector<std::string>{"build"}}};
    } else if (profile == "authorship/1") {
        value.subject_kind = liquid::attestation_subject_kind::drip;
        value.attributes = {{"author", "taap:1:ed25519:author"}};
    } else if (profile == "review_approval/1") {
        value.subject_kind = liquid::attestation_subject_kind::change;
        value.attributes = {{"reviewer", "taap:1:ed25519:reviewer"}, {"approved", true}};
    } else if (profile == "slsa_provenance/1") {
        const auto dependency = liquid::slsa_resource_descriptor{
            "liquid:blob:blake3:input", {{"blake3", "input"}}, std::nullopt};
        value.slsa_provenance = liquid::slsa_provenance_v1{
            .build_definition = {.build_type = "https://liquid.dev/build",
                                 .external_parameters = {{"liquid", "build"}, "", {}},
                                 .internal_parameters = {.toolchain = dependency,
                                                         .sandbox = dependency,
                                                         .platform = {{"os", "linux"},
                                                                      {"arch", "x86_64"}}},
                                 .resolved_dependencies = {dependency}},
            .run_details = {
                .builder = {"taap:1:ed25519:builder"},
                .metadata = {"invocation", "2026-01-01T00:00:00Z", "2026-01-01T00:00:01Z"},
                .byproducts = {dependency}}};
    } else if (profile == "sbom/1") {
        value.attributes = {{"format", "SPDX-2.3"},
                            {"output", "build/output.o"},
                            {"document", "blake3:document"},
                            {"derived", true},
                            {"inferred", false},
                            {"components", std::vector<std::string>{"pkg:cpp/liquid"}}};
    } else if (profile == "delta/1") {
        value.attributes = {{"source", "blake3:source"},
                            {"target", "blake3:target"},
                            {"algorithm", "zstd-patch"},
                            {"delta", "blake3:delta"}};
    } else if (profile == "hot_update/1") {
        value.attributes = {{"supersedes", "blake3:old"},
                            {"target", "blake3:new"},
                            {"tier", "1"},
                            {"symbols", std::vector<std::string>{"answer"}},
                            {"object", "blake3:object"},
                            {"patch", "blake3:patch"},
                            {"outcome", "patched"}};
    } else if (profile == "model_provenance/1") {
        value.subject_kind = liquid::attestation_subject_kind::model;
        value.attributes = {{"training_dataset", "blake3:dataset"},
                            {"training_code", "blake3:code"}};
    } else if (profile == "abi_compatibility/1") {
        value.subject_kind = liquid::attestation_subject_kind::library;
        value.attributes = {{"compatible_with", "liquid/0"}, {"compatibility", "backward"}};
    } else if (profile == "scan_result/1") {
        value.subject_kind = liquid::attestation_subject_kind::drip;
        value.detectors = {"aws-access-key"};
    } else if (profile == "reproducibility/1") {
        value.attributes = {{"verified_by", "taap:1:ed25519:verifier"},
                            {"sandbox", "blake3:sandbox"},
                            {"agreement", true}};
    } else if (profile == "image_provenance/1") {
        value.attributes = {{"image", "registry.example/liquid@sha256:image"},
                            {"tree", "blake3:tree"},
                            {"result", "blake3:result"}};
    }
    return value;
}

auto sign(liquid::attestation value) -> liquid::attestation {
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

auto sign_unknown_profile(liquid::attestation value) -> liquid::attestation {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    value.issuer = signer->identity();
    auto body = liquid::objects::encode_attestation_body_unchecked(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    value.signature = std::move(*signature);
    return value;
}

TEST(AttestationRegistry, all_profiles_accept_examples_and_reject_missing_required_fields) {
    const auto& registry = liquid::attestation_profile_registry::global();
    ASSERT_EQ(registry.profiles().size(), 12U);
    for (const auto& profile : registry.profiles()) {
        auto accepted = registry.validate(example(profile.name));
        ASSERT_TRUE(accepted) << profile.name << ": " << accepted.error().message;
        auto missing = example(profile.name);
        if (profile.name == "scan_result/1") {
            missing.detectors.clear();
        } else if (profile.name == "slsa_provenance/1") {
            missing.slsa_provenance.reset();
        } else {
            const auto required = std::ranges::find_if(
                profile.schema, [](const auto& field) { return field.required; });
            ASSERT_NE(required, profile.schema.end());
            std::erase_if(missing.attributes, [&required](const auto& attribute) {
                return attribute.name == required->name;
            });
        }
        auto refused = registry.validate(missing);
        EXPECT_FALSE(refused) << profile.name;
        auto encoded = liquid::encode_attestation(missing);
        EXPECT_FALSE(encoded) << profile.name;
    }
}

TEST(AttestationRegistry, rejects_unknown_profiles) {
    auto value = example("authorship/1");
    value.profile = "unregistered/1";
    auto verified = liquid::verify_attestation(sign_unknown_profile(std::move(value)));
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_FALSE(verified->verified);
    EXPECT_EQ(verified->reason, "unknown attestation profile: unregistered/1");
}

TEST(AttestationRegistry, applies_maximum_age_freshness) {
    const auto now = std::chrono::sys_seconds{std::chrono::seconds{2'000'000}};
    auto stale = example("scan_result/1");
    stale.at = now - std::chrono::hours{25};
    auto stale_verified = liquid::verify_attestation(sign(std::move(stale)), {.now = now});
    ASSERT_TRUE(stale_verified) << stale_verified.error().message;
    EXPECT_FALSE(stale_verified->verified);
    EXPECT_EQ(stale_verified->reason, "attestation is stale");

    auto fresh = example("scan_result/1");
    fresh.at = now - std::chrono::hours{23};
    auto fresh_verified = liquid::verify_attestation(sign(std::move(fresh)), {.now = now});
    ASSERT_TRUE(fresh_verified) << fresh_verified.error().message;
    EXPECT_TRUE(fresh_verified->verified) << fresh_verified->reason;
}

TEST(AttestationRegistry, verifies_selective_disclosure_commitments) {
    const auto components =
        liquid::attestation_attribute{"components", std::vector<std::string>{"pkg:cpp/liquid"}};
    const auto salt = std::vector<std::byte>{std::byte{1}, std::byte{2}};
    auto commitment = liquid::attribute_commitment(components, salt);
    ASSERT_TRUE(commitment) << commitment.error().message;

    auto value = example("sbom/1");
    value.attributes = {{"format", "SPDX-2.3"},
                        {"output", "build/output.o"},
                        {"document", "blake3:document"},
                        {"derived", true},
                        {"inferred", false}};
    value.disclosure_commitments = {{"components", *commitment}};
    value = sign(std::move(value));
    auto verified =
        liquid::verify_attestation(value, {.now = value.at, .disclosures = {{components, salt}}});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;

    auto wrong_salt = salt;
    wrong_salt.front() = std::byte{9};
    auto refused = liquid::verify_attestation(
        value, {.now = value.at, .disclosures = {{components, wrong_salt}}});
    ASSERT_TRUE(refused) << refused.error().message;
    EXPECT_FALSE(refused->verified);
    EXPECT_EQ(refused->reason, "attribute commitment mismatch");
}

TEST(AttestationRegistry, preserves_the_legacy_scan_result_digest) {
    auto value = example("scan_result/1");
    value.findings_count = 7;
    value.allowances = {{"config.env", "aws-access-key", 12, "allow-list"}};
    value.issuer = {"taap:1:ed25519:issuer", "ed25519", "issuer", {std::byte{7}}, "issuer"};
    value.signature = {std::byte{8}, std::byte{9}};
    auto encoded = liquid::objects::encode(value);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::objects::decode_attestation(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto reencoded = liquid::objects::encode(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
    auto digest = liquid::hash(liquid::algorithm::blake3, *encoded);
    ASSERT_TRUE(digest) << digest.error().message;
    EXPECT_EQ(liquid::to_string(*digest),
              "blake3:7bf3c617584910f326855bd9382c5bcc4b191dfd45eff381b5d9faf662c81423");
}

} // namespace
