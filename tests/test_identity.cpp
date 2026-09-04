#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "identity/taap.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "monocypher-ed25519.h"

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
    return std::filesystem::temp_directory_path() /
           ("liquid-identity-rotation-" + std::string{name});
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
        auto document = liquid::identity_detail::parse_document(bytes);
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
    const auto home = std::filesystem::temp_directory_path() / "liquid-identity-test";
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
    const auto path = std::filesystem::temp_directory_path() / "liquid-key-file-provider-test";
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
