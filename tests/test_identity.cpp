// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "encoding/cbor.hpp"
#include "identity/taap.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "monocypher-ed25519.h"
#include "temporary_directory.hpp"

namespace {

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto decode_hex(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    for (std::size_t index = 0; index < input.size(); index += 2) {
        const auto value = std::stoi(std::string{input.substr(index, 2)}, nullptr, 16);
        output.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    return output;
}

auto rotation_home(std::string_view name) -> std::filesystem::path {
    return liquid::test::fixture_path("liquid-identity-rotation-" + std::string{name});
}

auto set_identity_home(const std::filesystem::path& path) -> void {
    std::error_code code;
    std::filesystem::remove_all(path, code);
    setenv("LIQUID_HOME", path.c_str(), 1);
}

auto read_identity_documents(const std::filesystem::path& home, const liquid::actor& subject)
    -> std::vector<liquid::identity_document> {
    const auto directory = home / "identity" / subject.fingerprint / "documents";
    auto paths = std::vector<std::filesystem::path>{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".cbor") {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);
    auto documents = std::vector<liquid::identity_document>{};
    for (const auto& path : paths) {
        auto input = std::ifstream{path, std::ios::binary};
        auto bytes = std::vector<std::byte>{};
        for (char character; input.get(character);) {
            bytes.push_back(std::byte{static_cast<unsigned char>(character)});
        }
        auto document = liquid::decode_identity_document(bytes);
        EXPECT_TRUE(document) << document.error().message;
        if (document) {
            documents.push_back(std::move(*document));
        }
    }
    return documents;
}

auto expect_chain_error(const std::vector<liquid::identity_document>& documents,
                        std::string_view expected) -> void {
    const auto verified = liquid::verify_chain(documents);
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().message, expected);
}

TEST(Identity, Sha512_256NistVector) {
    const auto digest = liquid::identity_detail::sha512_256(bytes("abc"));
    const auto expected =
        decode_hex("53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
    EXPECT_TRUE(std::ranges::equal(digest, expected));
}

TEST(Identity, Ed25519Rfc8032Vector) {
    auto seed = decode_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const auto expected_public =
        decode_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const auto expected_signature =
        decode_hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                   "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    auto secret = std::array<std::byte, 64>{};
    auto public_key = std::array<std::byte, 32>{};
    crypto_ed25519_key_pair(reinterpret_cast<std::uint8_t*>(secret.data()),
                            reinterpret_cast<std::uint8_t*>(public_key.data()),
                            reinterpret_cast<std::uint8_t*>(seed.data()));
    auto signature = std::array<std::byte, 64>{};
    crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()),
                        reinterpret_cast<const std::uint8_t*>(secret.data()),
                        nullptr,
                        0);
    EXPECT_TRUE(std::ranges::equal(public_key, expected_public));
    EXPECT_TRUE(std::ranges::equal(signature, expected_signature));
    EXPECT_EQ(crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(signature.data()),
                                   reinterpret_cast<const std::uint8_t*>(public_key.data()),
                                   nullptr,
                                   0),
              0);
}

TEST(Identity, LocalSignerSignsAndVerifiesWithDomainSeparation) {
    const auto home = liquid::test::fixture_path("liquid-identity-test");
    std::error_code code;
    std::filesystem::remove_all(home, code);
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    const auto body = bytes("canonical body");
    auto signed_value = liquid::runtime::block_on(signer->sign("drip", body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    auto valid = liquid::verify(signer->identity(), "drip", body, *signed_value);
    ASSERT_TRUE(valid) << valid.error().message;
    EXPECT_TRUE(*valid);
    auto wrong_domain = liquid::verify(signer->identity(), "change", body, *signed_value);
    EXPECT_FALSE(wrong_domain);
    auto tampered = body;
    tampered.front() = std::byte{'C'};
    auto changed = liquid::verify(signer->identity(), "drip", tampered, *signed_value);
    EXPECT_FALSE(changed);
    std::filesystem::remove_all(home, code);
}

TEST(Identity, RotationStoresAndVerifiesTheKeyChain) {
    const auto home = rotation_home("valid");
    set_identity_home(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    const auto initial = signer->identity();
    const auto body = bytes("drip signed before rotation");
    const auto old_signature = liquid::runtime::block_on(signer->sign("drip", body));
    ASSERT_TRUE(old_signature) << old_signature.error().message;
    const auto successor = liquid::runtime::block_on(signer->rotate());
    ASSERT_TRUE(successor) << successor.error().message;
    EXPECT_EQ(successor->subject.identifier(), initial.identifier());
    EXPECT_NE(successor->subject.key_fingerprint, initial.key_fingerprint);
    const auto documents = read_identity_documents(home, initial);
    ASSERT_EQ(documents.size(), 2U);
    const auto verified = liquid::verify_chain(documents);
    ASSERT_TRUE(verified) << verified.error().message;
    ASSERT_EQ(verified->current_keys.size(), 1U);
    EXPECT_EQ(verified->current_keys.front().key_fingerprint, successor->subject.key_fingerprint);
    EXPECT_FALSE(verified->has_compromised_history);
    const auto old_drip_valid = liquid::verify(successor->subject, "drip", body, *old_signature);
    ASSERT_TRUE(old_drip_valid) << old_drip_valid.error().message;
    EXPECT_TRUE(*old_drip_valid);
    const auto reopened = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->identity().key_fingerprint, successor->subject.key_fingerprint);
    std::error_code code;
    std::filesystem::remove_all(home, code);
}

TEST(Identity, PublicCodecsRoundTripStoredIdentityAndRotationDocuments) {
    const auto home = rotation_home("public-codecs");
    set_identity_home(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    const auto initial = signer->identity();
    const auto successor = liquid::runtime::block_on(signer->rotate());
    ASSERT_TRUE(successor) << successor.error().message;

    const auto directory = home / "identity" / initial.fingerprint / "documents";
    auto document_count = std::size_t{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        auto input = std::ifstream{entry.path(), std::ios::binary};
        auto stored = std::vector<std::byte>{};
        for (char character; input.get(character);) {
            stored.push_back(std::byte{static_cast<unsigned char>(character)});
        }
        auto decoded = liquid::decode_identity_document(stored);
        ASSERT_TRUE(decoded) << decoded.error().message;
        auto encoded = liquid::encode_identity_document(*decoded);
        ASSERT_TRUE(encoded) << encoded.error().message;
        EXPECT_EQ(*encoded, stored);
        ++document_count;
    }
    EXPECT_EQ(document_count, 2U);
    std::error_code code;
    std::filesystem::remove_all(home, code);
}

TEST(Identity, PublicSignatureDecoderAcceptsCanonicalSignatureRecord) {
    namespace cbor = liquid::encoding::cbor;
    const auto encoded_bytes = std::string(86, 'A') + "==";
    auto fields = cbor::value::map{
        {"signature_algorithm", "ed25519"},
        {"signing_key_identifier", "taap:1:ed25519:TEST#TEST"},
        {"signing_key_label", "test"},
        {"signature_purpose", "drip"},
        {"signed_at", "2026-09-04T00:00:00Z"},
        {"signature_bytes_base64", encoded_bytes},
    };
    const auto encoded = cbor::encode(cbor::value{std::move(fields)});
    ASSERT_TRUE(encoded) << encoded.error().message;
    const auto decoded = liquid::decode_signature(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->algorithm, "ed25519");
    EXPECT_EQ(decoded->signature_purpose, "drip");
    EXPECT_EQ(decoded->bytes, std::vector<std::byte>(64));
}

TEST(Identity, RotationChainRejectsNamedIntegrityFailures) {
    const auto home = rotation_home("failures");
    set_identity_home(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    const auto initial = signer->identity();
    const auto successor = liquid::runtime::block_on(signer->rotate());
    ASSERT_TRUE(successor) << successor.error().message;
    const auto documents = read_identity_documents(home, initial);
    ASSERT_EQ(documents.size(), 2U);

    auto missing_dual_signature = documents;
    missing_dual_signature.back().signatures.erase(
        missing_dual_signature.back().signatures.begin());
    expect_chain_error(missing_dual_signature, "identity_chain_missing_dual_signature");

    auto non_monotonic_update = documents;
    non_monotonic_update.back().updated_at = non_monotonic_update.front().updated_at;
    expect_chain_error(non_monotonic_update, "identity_chain_non_monotonic_updated_at");

    auto broken_hash = documents;
    broken_hash.back().previous_document_hash->front() ^= std::byte{1};
    expect_chain_error(broken_hash, "identity_chain_broken_predecessor_hash");

    auto changed_identifier = documents;
    changed_identifier.back().subject.taap_id =
        "taap:1:ed25519:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    expect_chain_error(changed_identifier, "identity_chain_identifier_changed");
    std::error_code code;
    std::filesystem::remove_all(home, code);
}

TEST(Identity, CompromisedKeyRotationUsesNewSignatureAndFlagsHistory) {
    const auto home = rotation_home("compromised");
    set_identity_home(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    const auto initial = signer->identity();
    const auto body = bytes("drip signed before reported compromise");
    const auto old_signature = liquid::runtime::block_on(signer->sign("drip", body));
    ASSERT_TRUE(old_signature) << old_signature.error().message;
    const auto compromise = liquid::compromise_declaration{
        initial.key_fingerprint, old_signature->signed_at, "private key exposure"};
    const auto successor = liquid::runtime::block_on(signer->rotate(compromise));
    ASSERT_TRUE(successor) << successor.error().message;
    const auto documents = read_identity_documents(home, initial);
    ASSERT_EQ(documents.size(), 2U);
    ASSERT_EQ(documents.back().signatures.size(), 1U);
    const auto verified = liquid::verify_chain(documents);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->has_compromised_history);

    auto before_compromise = *old_signature;
    before_compromise.signed_at = "2000-01-01T00:00:00Z";
    const auto before_valid = liquid::verify(successor->subject, "drip", body, before_compromise);
    ASSERT_TRUE(before_valid) << before_valid.error().message;
    EXPECT_TRUE(*before_valid);

    const auto at_compromise = liquid::verify(initial, "drip", body, *old_signature);
    ASSERT_FALSE(at_compromise);
    EXPECT_EQ(at_compromise.error().code, liquid::errc::unauthorized);
    std::error_code code;
    std::filesystem::remove_all(home, code);
}

TEST(Identity, SigningCostMeasurement) {
    const auto body = bytes("canonical drip body used for the local signing measurement");
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    constexpr auto count = 1000U;
    const auto started = std::chrono::steady_clock::now();
    for (auto index = 0U; index < count; ++index) {
        auto signed_value = liquid::runtime::block_on(signer->sign("drip", body));
        ASSERT_TRUE(signed_value) << signed_value.error().message;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::cout << "TAAP/1 Ed25519 signing mean: " << nanoseconds / count << " ns\n";
}

} // namespace

TEST(Identity, Argon2iDerivationIsDeterministicAndSalted) {
    liquid::passphrase_provider provider{"correct horse battery staple"};
    std::array<std::byte, 16> first_salt{};
    std::array<std::byte, 16> second_salt{};
    second_salt[0] = std::byte{1};
    const liquid::argon2_parameters parameters{8, 3, 1};
    const auto first = provider.derive(first_salt, parameters);
    const auto repeated = provider.derive(first_salt, parameters);
    const auto second = provider.derive(second_salt, parameters);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(repeated.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *repeated);
    EXPECT_NE(*first, *second);
}

TEST(Identity, KeyFileProviderRequiresAnExactPrivateKeyFile) {
    const auto path = liquid::test::fixture_path("liquid-key-file-provider-test");
    std::filesystem::remove(path);
    std::array<std::byte, 32> key{};
    key[0] = std::byte{0x42};
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(key.data()),
                     static_cast<std::streamsize>(key.size()));
    }
#if !defined(_WIN32)
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    const auto opened = liquid::runtime::block_on(liquid::key_file_provider::open(path));
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
#if !defined(_WIN32)
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::replace);
    const auto exposed = liquid::runtime::block_on(liquid::key_file_provider::open(path));
    ASSERT_FALSE(exposed.has_value());
    EXPECT_EQ(exposed.error().code, liquid::errc::unauthorized);
#endif
    std::filesystem::remove(path);
}

namespace {

auto open_signer_in(const std::filesystem::path& home) -> liquid::signer {
    set_identity_home(home);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    return std::move(*signer);
}

} // namespace

// liquid#383: a signer name is bound to a key only through that name's identity chain. A
// signature by key B that claims to be identity A must not verify where A's chain is unknown.
TEST(Identity, SignerNameWithoutAChainDoesNotVerify) {
    const auto victim_home = rotation_home("unknown-victim");
    const auto attacker_home = rotation_home("unknown-attacker");
    const auto verifier_home = rotation_home("unknown-verifier");
    auto victim = open_signer_in(victim_home);
    auto attacker = open_signer_in(attacker_home);
    const auto body = bytes("drip body");
    auto signed_value = liquid::runtime::block_on(attacker.sign("drip", body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;

    auto forged = attacker.identity();
    forged.taap_id = victim.identity().taap_id;
    forged.fingerprint = victim.identity().fingerprint;
    set_identity_home(verifier_home);
    const auto impersonation = liquid::verify(forged, "drip", body, *signed_value);
    ASSERT_FALSE(impersonation) << "a key verified as another identity";
    EXPECT_EQ(impersonation.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::is_unknown_signer(impersonation.error())) << impersonation.error().message;
    const auto stranger = liquid::verify(attacker.identity(), "drip", body, *signed_value);
    ASSERT_FALSE(stranger) << "an unknown identity verified";
    EXPECT_TRUE(liquid::is_unknown_signer(stranger.error())) << stranger.error().message;

    // Trusting the victim's real chain does not make the attacker's key the victim's.
    const auto victim_document = victim.document();
    ASSERT_TRUE(victim_document) << victim_document.error().message;
    ASSERT_TRUE(liquid::trust_identity_chain(std::array{*victim_document}));
    const auto still_forged = liquid::verify(forged, "drip", body, *signed_value);
    ASSERT_FALSE(still_forged);
    EXPECT_FALSE(liquid::is_unknown_signer(still_forged.error()));
    EXPECT_EQ(still_forged.error().message, "signing key is not in the identity chain");

    // Once the attacker's own chain is trusted, its signature verifies as the attacker only.
    const auto attacker_document = attacker.document();
    ASSERT_TRUE(attacker_document) << attacker_document.error().message;
    ASSERT_TRUE(liquid::trust_identity_chain(std::array{*attacker_document}));
    const auto honest = liquid::verify(attacker.identity(), "drip", body, *signed_value);
    ASSERT_TRUE(honest) << honest.error().message;
    EXPECT_TRUE(*honest);

    std::error_code code;
    std::filesystem::remove_all(victim_home, code);
    std::filesystem::remove_all(attacker_home, code);
    std::filesystem::remove_all(verifier_home, code);
}

// A document that claims another identity's name with its own key is self-consistent and
// self-signed, but its identifier is not derived from its key, so it binds nothing.
TEST(Identity, InitialDocumentMustBeNamedAfterItsOwnKey) {
    const auto victim_home = rotation_home("named-victim");
    const auto attacker_home = rotation_home("named-attacker");
    auto victim = open_signer_in(victim_home);
    auto attacker = open_signer_in(attacker_home);
    auto forged = attacker.document();
    ASSERT_TRUE(forged) << forged.error().message;
    forged->subject.taap_id = victim.identity().taap_id;
    forged->subject.fingerprint = victim.identity().fingerprint;
    auto body = liquid::identity_detail::canonical_document(*forged, false);
    ASSERT_TRUE(body) << body.error().message;
    auto resigned = liquid::runtime::block_on(attacker.sign("identity_document", *body));
    ASSERT_TRUE(resigned) << resigned.error().message;
    forged->signatures = {*resigned};

    EXPECT_FALSE(liquid::verify_document(*forged));
    expect_chain_error({*forged}, "identity_chain_identifier_not_derived_from_key");
    EXPECT_FALSE(liquid::trust_identity_chain(std::array{*forged}));
    const auto drip = bytes("drip body");
    auto signed_value = liquid::runtime::block_on(attacker.sign("drip", drip));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    EXPECT_FALSE(liquid::verify(forged->subject, std::array{*forged}, "drip", drip, *signed_value));

    std::error_code code;
    std::filesystem::remove_all(victim_home, code);
    std::filesystem::remove_all(attacker_home, code);
}

TEST(Identity, ChainTravelsAndIsTrustedOnlyByExtension) {
    const auto author_home = rotation_home("travel-author");
    const auto verifier_home = rotation_home("travel-verifier");
    auto author = open_signer_in(author_home);
    const auto initial = author.document();
    ASSERT_TRUE(initial) << initial.error().message;
    auto successor = liquid::runtime::block_on(author.rotate());
    ASSERT_TRUE(successor) << successor.error().message;
    auto chain = liquid::load_identity_chain(author.identity().taap_id);
    ASSERT_TRUE(chain) << chain.error().message;
    ASSERT_EQ(chain->size(), 2U);

    auto encoded = liquid::encode_identity_chain(*chain);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::decode_identity_chain(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded->size(), 2U);
    auto trailing = *encoded;
    trailing.push_back(std::byte{0});
    EXPECT_FALSE(liquid::decode_identity_chain(trailing));

    const auto drip = bytes("drip body");
    auto signed_value = liquid::runtime::block_on(author.sign("drip", drip));
    ASSERT_TRUE(signed_value) << signed_value.error().message;

    set_identity_home(verifier_home);
    const auto carried = liquid::verify(author.identity(), *decoded, "drip", drip, *signed_value);
    ASSERT_TRUE(carried) << carried.error().message;
    EXPECT_TRUE(*carried);
    const auto unknown = liquid::verify(author.identity(), "drip", drip, *signed_value);
    ASSERT_FALSE(unknown);
    EXPECT_TRUE(liquid::is_unknown_signer(unknown.error()));
    const auto missing = liquid::load_identity_chain(author.identity().taap_id);
    ASSERT_FALSE(missing);
    EXPECT_TRUE(liquid::is_unknown_signer(missing.error()));

    // A prefix is trusted, then extended; a shorter chain afterwards changes nothing.
    ASSERT_TRUE(liquid::trust_identity_chain(std::array{*initial}));
    const auto current_key_unknown = liquid::verify(author.identity(), "drip", drip, *signed_value);
    ASSERT_FALSE(current_key_unknown);
    EXPECT_EQ(current_key_unknown.error().message, "signing key is not in the identity chain");
    ASSERT_TRUE(liquid::trust_identity_chain(*decoded));
    const auto trusted = liquid::verify(author.identity(), "drip", drip, *signed_value);
    ASSERT_TRUE(trusted) << trusted.error().message;
    EXPECT_TRUE(*trusted);
    ASSERT_TRUE(liquid::trust_identity_chain(std::array{*initial}));
    auto stored = liquid::load_identity_chain(author.identity().taap_id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(stored->size(), 2U);

    std::error_code code;
    std::filesystem::remove_all(author_home, code);
    std::filesystem::remove_all(verifier_home, code);
}

// Two different successors to one initial document are a fork: whichever was stored first stays,
// and the other is refused rather than silently replacing it.
TEST(Identity, ForkedChainIsRefused) {
    const auto author_home = rotation_home("fork-author");
    const auto copy_home = rotation_home("fork-copy");
    const auto verifier_home = rotation_home("fork-verifier");
    auto author = open_signer_in(author_home);
    std::error_code code;
    std::filesystem::copy(author_home, copy_home, std::filesystem::copy_options::recursive, code);
    ASSERT_FALSE(code) << code.message();
    ASSERT_TRUE(liquid::runtime::block_on(author.rotate()));
    auto first_chain = liquid::load_identity_chain(author.identity().taap_id);
    ASSERT_TRUE(first_chain) << first_chain.error().message;

    setenv("LIQUID_HOME", copy_home.c_str(), 1);
    auto copy = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(copy) << copy.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(copy->rotate()));
    auto second_chain = liquid::load_identity_chain(copy->identity().taap_id);
    ASSERT_TRUE(second_chain) << second_chain.error().message;
    ASSERT_EQ(first_chain->front().subject.taap_id, second_chain->front().subject.taap_id);

    set_identity_home(verifier_home);
    ASSERT_TRUE(liquid::trust_identity_chain(*first_chain));
    const auto fork = liquid::trust_identity_chain(*second_chain);
    ASSERT_FALSE(fork);
    EXPECT_EQ(fork.error().code, liquid::errc::conflict);

    std::filesystem::remove_all(author_home, code);
    std::filesystem::remove_all(copy_home, code);
    std::filesystem::remove_all(verifier_home, code);
}

// A rotation that only declares the old key compromised is signed by its new key alone. Anyone can
// write one for any public initial document, so it is never accepted from a remote chain.
TEST(Identity, CompromiseOnlyRotationIsNotTrustedFromAnotherMachine) {
    const auto author_home = rotation_home("compromise-author");
    const auto verifier_home = rotation_home("compromise-verifier");
    auto author = open_signer_in(author_home);
    const auto initial = author.identity();
    const auto drip = bytes("drip body");
    auto rotated = liquid::runtime::block_on(author.rotate(
        liquid::compromise_declaration{initial.key_fingerprint, "2099-01-01T00:00:00Z", {}}));
    ASSERT_TRUE(rotated) << rotated.error().message;
    auto signed_value = liquid::runtime::block_on(author.sign("drip", drip));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    auto chain = liquid::load_identity_chain(initial.taap_id);
    ASSERT_TRUE(chain) << chain.error().message;
    ASSERT_EQ(chain->size(), 2U);
    const auto local = liquid::verify(author.identity(), "drip", drip, *signed_value);
    ASSERT_TRUE(local) << local.error().message;

    set_identity_home(verifier_home);
    EXPECT_FALSE(liquid::verify(author.identity(), *chain, "drip", drip, *signed_value));
    const auto refused = liquid::trust_identity_chain(*chain);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_FALSE(liquid::load_identity_chain(initial.taap_id));
    ASSERT_TRUE(liquid::trust_identity_chain(*chain, {.accept_compromise_rotation = true}));
    const auto accepted = liquid::verify(author.identity(), "drip", drip, *signed_value);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(*accepted);

    std::error_code code;
    std::filesystem::remove_all(author_home, code);
    std::filesystem::remove_all(verifier_home, code);
}

TEST(Identity, SignerNameIsNotAPath) {
    const auto home = rotation_home("name-path");
    set_identity_home(home);
    auto value = liquid::actor{};
    value.taap_id = "taap:1:ed25519:../../escape";
    value.fingerprint = "../../escape";
    const auto signed_value = liquid::signature{};
    const auto result = liquid::verify(value, "drip", bytes("body"), signed_value);
    ASSERT_FALSE(result);
    EXPECT_TRUE(liquid::is_unknown_signer(result.error()));
    EXPECT_FALSE(liquid::load_identity_chain(value.taap_id));
    std::error_code code;
    std::filesystem::remove_all(home, code);
}
