// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

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
        value.issuer = liquid::actor{.taap_id = "taap:1:ed25519:principal"};
        value.subject =
            *liquid::delegation_subject(liquid::actor{.taap_id = "taap:1:ed25519:agent"});
        value.attributes = {{"scopes", std::vector<std::string>{"src/**"}},
                            {"may_publish", true},
                            {"may_lease", true},
                            {"depth_remaining", std::uint64_t{2}},
                            {"model", "liquid-agent"},
                            {"dispatcher", "taap:1:ed25519:dispatcher"},
                            {"not_after", std::uint64_t{3700}},
                            {"repository", "00112233445566778899aabbccddeeff"}};
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
    } else if (profile == "projection/1") {
        value.attributes = {{"target", "oci"},
                            {"source_result", std::vector<std::string>{"blake3:result"}},
                            {"normalisation", "epoch,uid=0,gid=0,modes,sorted"},
                            {"tool", "liquid/test"}};
    } else if (profile == "platform_signature/1") {
        value.attributes = {{"platform", "macos"},
                            {"unsigned_digest", "sha256:unsigned"},
                            {"signed_digest", "sha256:signed"},
                            {"signing_identity", "SHA256:fingerprint"},
                            {"verification", "accepted"},
                            {"notarization_ticket", "ticket-id"}};
    } else if (profile == "release_policy/1") {
        value.subject_kind = liquid::attestation_subject_kind::repository;
        value.attributes = {{"role", "targets"},
                            {"threshold", std::uint64_t{2}},
                            {"signers", std::vector<std::string>{"taap:one", "taap:two"}},
                            {"not_after", "2099-01-01T00:00:00Z"}};
    } else if (profile == "release_approval/1") {
        value.attributes = {{"policy", "blake3:policy"}, {"release", "v1"}};
    }
    return value;
}

// A sub-delegation of `parent`: issued by the agent `parent` delegates to, to a further agent,
// with the parent's attributes until a test changes them.
auto child_of(const liquid::attestation& parent) -> liquid::attestation {
    auto child = parent;
    child.issuer = liquid::actor{.taap_id = "taap:1:ed25519:agent"};
    child.subject =
        *liquid::delegation_subject(liquid::actor{.taap_id = "taap:1:ed25519:subagent"});
    return child;
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

auto select_home(std::string_view name) -> std::filesystem::path {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("liquid-attestation-home-" + std::to_string(::getpid()) + "-" + std::string{name});
    std::error_code code;
    std::filesystem::remove_all(path, code);
    setenv("LIQUID_HOME", path.c_str(), 1);
    return path;
}

TEST(AttestationRegistry, all_profiles_accept_examples_and_reject_missing_required_fields) {
    const auto& registry = liquid::attestation_profile_registry::global();
    ASSERT_EQ(registry.profiles().size(), 17U);
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

TEST(AttestationRegistry, delegation_chains_only_attenuate_authority) {
    auto parent = example("delegation/1");
    auto child = child_of(parent);
    child.attributes = {{"scopes", std::vector<std::string>{"src/library/**"}},
                        {"may_publish", false},
                        {"may_lease", true},
                        {"depth_remaining", std::uint64_t{1}},
                        {"dispatcher", "taap:1:ed25519:dispatcher"},
                        {"not_after", std::uint64_t{3700}},
                        {"repository", "00112233445566778899aabbccddeeff"}};
    EXPECT_TRUE(liquid::validate_delegation(child, &parent));

    auto broad = child;
    broad.attributes[0].value = std::vector<std::string>{"docs/**"};
    EXPECT_FALSE(liquid::validate_delegation(broad, &parent));

    auto publishing = child;
    publishing.attributes[1].value = true;
    parent.attributes[1].value = false;
    EXPECT_FALSE(liquid::validate_delegation(publishing, &parent));
}

// docs/security/delegation-threat-model.md, must-never D1: only a complete, unambiguous
// delegation/1 record is delegation authority.
TEST(AttestationRegistry, delegation_refuses_other_profiles_and_incomplete_authority) {
    const auto other = example("authorship/1");
    EXPECT_FALSE(liquid::validate_delegation(other));

    const auto parent = example("delegation/1");
    auto child = child_of(parent);
    child.attributes[3].value = std::uint64_t{1};
    EXPECT_TRUE(liquid::validate_delegation(child, &parent));
    EXPECT_FALSE(liquid::validate_delegation(child, &other));

    auto incomplete = example("delegation/1");
    std::erase_if(incomplete.attributes,
                  [](const auto& attribute) { return attribute.name == "may_lease"; });
    EXPECT_FALSE(liquid::validate_delegation(incomplete));

    auto duplicated = example("delegation/1");
    duplicated.attributes.push_back({"scopes", std::vector<std::string>{"**"}});
    EXPECT_FALSE(liquid::validate_delegation(duplicated));

    auto mistyped = example("delegation/1");
    mistyped.attributes[1].value = std::string{"true"};
    EXPECT_FALSE(liquid::validate_delegation(mistyped));

    auto wrong_subject = example("delegation/1");
    wrong_subject.subject_kind = liquid::attestation_subject_kind::drip;
    EXPECT_FALSE(liquid::validate_delegation(wrong_subject));
}

// docs/security/delegation-threat-model.md, must-never D5: a sub-delegation reduces
// depth_remaining, an exhausted delegation cannot delegate, and no capability is added.
TEST(AttestationRegistry, sub_delegation_reduces_depth_and_never_adds_leases) {
    const auto parent = example("delegation/1");

    const auto same_depth = child_of(parent);
    EXPECT_FALSE(liquid::validate_delegation(same_depth, &parent));

    auto deeper = child_of(parent);
    deeper.attributes[3].value = std::uint64_t{3};
    EXPECT_FALSE(liquid::validate_delegation(deeper, &parent));

    auto reduced = child_of(parent);
    reduced.attributes[3].value = std::uint64_t{1};
    EXPECT_TRUE(liquid::validate_delegation(reduced, &parent));

    auto exhausted = parent;
    exhausted.attributes[3].value = std::uint64_t{0};
    const auto from_exhausted = child_of(exhausted);
    EXPECT_FALSE(liquid::validate_delegation(from_exhausted, &exhausted));

    auto no_lease_parent = parent;
    no_lease_parent.attributes[2].value = false;
    auto leasing = reduced;
    EXPECT_FALSE(liquid::validate_delegation(leasing, &no_lease_parent));
    leasing.attributes[2].value = false;
    EXPECT_TRUE(liquid::validate_delegation(leasing, &no_lease_parent));
}

// docs/security/delegation-threat-model.md, must-never D4 (liquid#514): a delegation is valid only
// inside its window, which is bounded and has an end.
TEST(AttestationRegistry, delegation_window_refuses_expired_future_and_unbounded_delegations) {
    const auto value = example("delegation/1");
    const auto issued = value.at;
    EXPECT_TRUE(liquid::check_delegation_window(value, issued));
    EXPECT_TRUE(liquid::check_delegation_window(value, issued + std::chrono::seconds{3599}));

    auto expired = liquid::check_delegation_window(value, issued + std::chrono::seconds{3600});
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, liquid::errc::unauthorized);

    auto early = liquid::check_delegation_window(
        value, issued - liquid::delegation_clock_skew - std::chrono::seconds{1});
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::check_delegation_window(value, issued - liquid::delegation_clock_skew));

    const auto not_after = [](liquid::attestation& target) -> liquid::attestation_value& {
        return std::ranges::find(target.attributes,
                                 std::string_view{"not_after"},
                                 &liquid::attestation_attribute::name)
            ->value;
    };
    auto unbounded = value;
    not_after(unbounded) = std::uint64_t{
        100 + static_cast<std::uint64_t>(liquid::delegation_maximum_lifetime.count()) + 1};
    auto too_long = liquid::check_delegation_window(unbounded, issued);
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code, liquid::errc::invalid_argument);

    auto backwards = value;
    not_after(backwards) = std::uint64_t{100};
    EXPECT_FALSE(liquid::check_delegation_window(backwards, issued));

    auto undated = value;
    std::erase_if(undated.attributes,
                  [](const auto& attribute) { return attribute.name == "not_after"; });
    EXPECT_FALSE(liquid::check_delegation_window(undated, issued));
    EXPECT_FALSE(liquid::validate_delegation(undated));
}

// liquid#514: the local revocation list revokes exactly the digests it lists, a missing list
// revokes nothing, and a damaged list refuses instead of revoking nothing.
TEST(AttestationRegistry, local_delegation_status_reads_the_revocation_list) {
    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto home = std::filesystem::temp_directory_path() /
                      ("liquid-revocations-" + std::to_string(::getpid()));
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home / "trust");
    setenv("LIQUID_HOME", home.c_str(), 1);

    const auto value = example("delegation/1");
    auto listed = liquid::hash(liquid::algorithm::blake3, std::span<const std::byte>{});
    auto other = liquid::digest{liquid::algorithm::blake3, {}};
    ASSERT_TRUE(listed);
    const auto status = liquid::local_delegation_status();

    auto missing = status(value, *listed);
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_FALSE(missing->revoked);

    {
        auto output = std::ofstream{home / "trust" / "delegation-revocations"};
        output << "# revoked delegations\n\n" << liquid::to_string(*listed) << "  # leaked\n";
    }
    auto revoked = status(value, *listed);
    ASSERT_TRUE(revoked) << revoked.error().message;
    EXPECT_TRUE(revoked->revoked);
    auto kept = status(value, other);
    ASSERT_TRUE(kept) << kept.error().message;
    EXPECT_FALSE(kept->revoked);

    {
        auto output = std::ofstream{home / "trust" / "delegation-revocations", std::ios::app};
        output << "not a digest\n";
    }
    EXPECT_FALSE(status(value, other));

    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    std::filesystem::remove_all(home);
}

// docs/security/delegation-threat-model.md, must-never D6 (liquid#517): a sub-delegation's scopes
// never match a path its parent's scopes do not. Containment is decided conservatively, so a
// wildcard prefix never admits a child.
TEST(AttestationRegistry, sub_delegation_scopes_are_contained_conservatively) {
    const auto scoped = [](std::vector<std::string> parent_scopes,
                           std::vector<std::string> child_scopes) {
        auto parent = example("delegation/1");
        parent.attributes[0].value = std::move(parent_scopes);
        auto child = child_of(parent);
        child.attributes[0].value = std::move(child_scopes);
        child.attributes[3].value = std::uint64_t{1};
        return liquid::validate_delegation(child, &parent).has_value();
    };
    EXPECT_FALSE(scoped({"src/*.cpp"}, {"src/secret/**"}));
    EXPECT_FALSE(scoped({"src/*"}, {"src/**"}));
    EXPECT_FALSE(scoped({"src/a?"}, {"src/a/**"}));
    EXPECT_FALSE(scoped({"src/**"}, {"srcs/**"}));
    EXPECT_FALSE(scoped({"src/**"}, {"**"}));
    EXPECT_FALSE(scoped({"src/*.cpp"}, {"src/main.cpp"}));

    EXPECT_TRUE(scoped({"src/*.cpp"}, {"src/*.cpp"}));
    EXPECT_TRUE(scoped({"src/**"}, {"src/secret/**", "src/main.cpp", "src/*.cpp"}));
    EXPECT_TRUE(scoped({"**"}, {"docs/*.md"}));
    EXPECT_TRUE(scoped({"docs/**", "src/**"}, {"docs/guide.md", "src/lib/**"}));
}

// docs/security/delegation-threat-model.md, must-never D7 (liquid#517): a sub-delegation is linked
// to its parent: issued by the parent's subject, for the same repository, inside the parent's
// validity.
TEST(AttestationRegistry, sub_delegation_must_be_linked_to_its_parent) {
    const auto parent = example("delegation/1");
    auto child = child_of(parent);
    child.attributes[3].value = std::uint64_t{1};
    EXPECT_TRUE(liquid::validate_delegation(child, &parent));

    auto unlinked = child;
    unlinked.issuer = liquid::actor{.taap_id = "taap:1:ed25519:stranger"};
    EXPECT_FALSE(liquid::validate_delegation(unlinked, &parent));

    auto self_issued = child;
    self_issued.issuer = parent.issuer;
    EXPECT_FALSE(liquid::validate_delegation(self_issued, &parent));

    auto elsewhere = child;
    std::ranges::find(
        elsewhere.attributes, std::string_view{"repository"}, &liquid::attestation_attribute::name)
        ->value = std::string{"ffeeddccbbaa99887766554433221100"};
    EXPECT_FALSE(liquid::validate_delegation(elsewhere, &parent));

    auto earlier = child;
    earlier.at = parent.at - std::chrono::seconds{1};
    EXPECT_FALSE(liquid::validate_delegation(earlier, &parent));

    auto outliving = child;
    std::ranges::find(
        outliving.attributes, std::string_view{"not_after"}, &liquid::attestation_attribute::name)
        ->value = std::uint64_t{3701};
    EXPECT_FALSE(liquid::validate_delegation(outliving, &parent));
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

// liquid#383: an attestation's issuer is a name; it is bound to a key only by that name's identity
// chain. A verifier that does not hold the issuer's chain must refuse the attestation.
TEST(AttestationRegistry, refuses_an_issuer_whose_identity_chain_is_unavailable) {
    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto issuer_home = select_home("issuer");
    const auto signed_value = sign(example("authorship/1"));
    const auto verifier_home = select_home("verifier");
    auto verified = liquid::verify_attestation(signed_value);
    EXPECT_FALSE(verified && verified->verified)
        << "an attestation verified without its issuer's identity chain";
    std::error_code code;
    std::filesystem::remove_all(issuer_home, code);
    std::filesystem::remove_all(verifier_home, code);
    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
}

// liquid#512, liquid#545: `attest show` checks the issuer through the public
// `check_attestation_signature`. With the issuer's chain the identity is verified; without it the
// signature is still reported valid, by an unverified key.
TEST(AttestationRegistry, signature_check_reports_an_unchained_issuer_as_an_unverified_key) {
    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto issuer_home = select_home("check-issuer");
    const auto signed_value = sign(example("authorship/1"));
    auto chained = liquid::check_attestation_signature(signed_value);
    ASSERT_TRUE(chained) << chained.error().message;
    EXPECT_TRUE(chained->signature_valid);
    EXPECT_TRUE(chained->identity_verified);

    const auto verifier_home = select_home("check-verifier");
    auto unchained = liquid::check_attestation_signature(signed_value);
    ASSERT_TRUE(unchained) << unchained.error().message;
    EXPECT_TRUE(unchained->signature_valid);
    EXPECT_FALSE(unchained->identity_verified);

    auto tampered = signed_value;
    tampered.at += std::chrono::seconds{1};
    auto refused = liquid::check_attestation_signature(tampered);
    EXPECT_FALSE(refused && refused->signature_valid);
    std::error_code code;
    std::filesystem::remove_all(issuer_home, code);
    std::filesystem::remove_all(verifier_home, code);
    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
}

} // namespace
