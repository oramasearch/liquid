// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "crypto/signature.hpp"
#include "liquid/transparency.hpp"

namespace {

auto read_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> output(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(output.data()), size);
    return output;
}

auto read_text(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

auto fixture(std::string_view name) -> std::filesystem::path {
    return std::filesystem::path(LIQUID_TRANSPARENCY_FIXTURES) / name;
}

auto fixture_trust() -> liquid::x509::trust_anchor_set {
    liquid::x509::trust_anchor_set trust;
    const auto added = trust.add_pem(read_text(fixture("tsa-root.pem")));
    EXPECT_TRUE(added) << added.error().message;
    return trust;
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const std::byte value : input) {
        const auto byte = std::to_integer<unsigned int>(value);
        output.push_back(alphabet[byte >> 4]);
        output.push_back(alphabet[byte & 0x0f]);
    }
    return output;
}

template <class T, auto Free> struct openssl_deleter {
    auto operator()(T* value) const -> void {
        Free(value);
    }
};

using openssl_key = std::unique_ptr<EVP_PKEY, openssl_deleter<EVP_PKEY, EVP_PKEY_free>>;
using openssl_key_context =
    std::unique_ptr<EVP_PKEY_CTX, openssl_deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using openssl_digest_context =
    std::unique_ptr<EVP_MD_CTX, openssl_deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using openssl_number = std::unique_ptr<BIGNUM, openssl_deleter<BIGNUM, BN_free>>;

auto generate_key(int identifier, int curve = 0) -> openssl_key {
    auto context = openssl_key_context{EVP_PKEY_CTX_new_id(identifier, nullptr)};
    EVP_PKEY* output = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1) {
        return {};
    }
    if (identifier == EVP_PKEY_RSA && EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1) {
        return {};
    }
    if (identifier == EVP_PKEY_EC &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), curve) != 1) {
        return {};
    }
    if (EVP_PKEY_keygen(context.get(), &output) != 1) {
        return {};
    }
    return openssl_key{output};
}

auto number_bytes(const BIGNUM* value) -> std::vector<std::byte> {
    std::vector<std::byte> output(static_cast<std::size_t>(BN_num_bytes(value)));
    BN_bn2bin(value, reinterpret_cast<unsigned char*>(output.data()));
    return output;
}

auto rsa_public_key(EVP_PKEY* value) -> std::optional<liquid::crypto::public_key> {
    BIGNUM* modulus_value = nullptr;
    BIGNUM* exponent_value = nullptr;
    if (EVP_PKEY_get_bn_param(value, OSSL_PKEY_PARAM_RSA_N, &modulus_value) != 1 ||
        EVP_PKEY_get_bn_param(value, OSSL_PKEY_PARAM_RSA_E, &exponent_value) != 1) {
        BN_free(modulus_value);
        BN_free(exponent_value);
        return std::nullopt;
    }
    auto modulus = openssl_number{modulus_value};
    auto exponent = openssl_number{exponent_value};
    return liquid::crypto::public_key{
        .kind = liquid::crypto::signature_kind::rsa_pkcs1,
        .first = number_bytes(modulus.get()),
        .second = number_bytes(exponent.get()),
    };
}

auto ec_public_key(EVP_PKEY* value, liquid::crypto::curve_kind curve, std::size_t coordinate_size)
    -> std::optional<liquid::crypto::public_key> {
    std::size_t size = 0;
    if (EVP_PKEY_get_octet_string_param(value, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &size) != 1) {
        return std::nullopt;
    }
    std::vector<std::byte> encoded(size);
    if (EVP_PKEY_get_octet_string_param(value,
                                        OSSL_PKEY_PARAM_PUB_KEY,
                                        reinterpret_cast<unsigned char*>(encoded.data()),
                                        encoded.size(),
                                        &size) != 1 ||
        size != 1 + coordinate_size * 2 || encoded.front() != std::byte{4}) {
        return std::nullopt;
    }
    return liquid::crypto::public_key{
        .kind = liquid::crypto::signature_kind::ecdsa,
        .curve = curve,
        .first = {encoded.begin() + 1,
                  encoded.begin() + static_cast<std::ptrdiff_t>(1 + coordinate_size)},
        .second = {encoded.begin() + static_cast<std::ptrdiff_t>(1 + coordinate_size),
                   encoded.end()},
    };
}

auto sign(EVP_PKEY* key, const EVP_MD* digest, std::span<const std::byte> message, bool pss = false)
    -> std::vector<std::byte> {
    auto context = openssl_digest_context{EVP_MD_CTX_new()};
    EVP_PKEY_CTX* key_context = nullptr;
    if (!context || EVP_DigestSignInit(context.get(), &key_context, digest, nullptr, key) != 1) {
        return {};
    }
    if (pss && (EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) != 1 ||
                EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, digest) != 1 ||
                EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, EVP_MD_get_size(digest)) != 1)) {
        return {};
    }
    std::size_t size = 0;
    if (EVP_DigestSign(context.get(),
                       nullptr,
                       &size,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return {};
    }
    std::vector<std::byte> output(size);
    if (EVP_DigestSign(context.get(),
                       reinterpret_cast<unsigned char*>(output.data()),
                       &size,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return {};
    }
    output.resize(size);
    return output;
}

} // namespace

TEST(TransparencyTimestamp, VerifiesLakeInitialEvidence) {
    const auto record = read_bytes(fixture("lake-initial.ers"));
    const auto checkpoint_signature = read_bytes(fixture("checkpoint-signature.bin"));
    auto trust = fixture_trust();
    const auto verified =
        liquid::transparency::evidence_record::verify(record, checkpoint_signature, trust);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified->digest_algorithm, "sha256");
    ASSERT_EQ(verified->archive_timestamps.size(), 1U);
    const auto& timestamp = verified->archive_timestamps.front().timestamp;
    EXPECT_EQ(timestamp.policy, "1.3.6.1.4.1.57264.1");
    EXPECT_EQ(timestamp.digest_algorithm, "sha256");
    EXPECT_EQ(timestamp.signature_algorithm, "ecdsa");
    EXPECT_FALSE(timestamp.nonce.empty());
    ASSERT_EQ(timestamp.certificate_path.size(), 2U);
    EXPECT_TRUE(timestamp.signer.digital_signature);
    EXPECT_TRUE(timestamp.signer.timestamp_signing);
}

TEST(TransparencyTimestamp, VerifiesLakeRenewalAndPreservesPredecessor) {
    const auto initial = read_bytes(fixture("lake-initial.ers"));
    const auto renewed = read_bytes(fixture("lake-renewed.ers"));
    const auto checkpoint_signature = read_bytes(fixture("checkpoint-signature.bin"));
    auto trust = fixture_trust();
    const auto initial_verified =
        liquid::transparency::evidence_record::verify(initial, checkpoint_signature, trust);
    const auto renewed_verified =
        liquid::transparency::evidence_record::verify(renewed, checkpoint_signature, trust);
    ASSERT_TRUE(initial_verified) << initial_verified.error().message;
    ASSERT_TRUE(renewed_verified) << renewed_verified.error().message;
    ASSERT_EQ(renewed_verified->archive_timestamps.size(), 2U);
    EXPECT_EQ(renewed_verified->archive_timestamps.front().encoded,
              initial_verified->archive_timestamps.front().encoded);
    EXPECT_LE(renewed_verified->archive_timestamps.front().timestamp.generated_at,
              renewed_verified->archive_timestamps.back().timestamp.generated_at);
}

TEST(TransparencyTimestamp, RejectsUntrustedChangedAndBrokenEvidence) {
    const auto checkpoint_signature = read_bytes(fixture("checkpoint-signature.bin"));
    const auto initial = read_bytes(fixture("lake-initial.ers"));
    const auto renewed = read_bytes(fixture("lake-renewed.ers"));

    liquid::x509::trust_anchor_set no_trust;
    const auto untrusted =
        liquid::transparency::evidence_record::verify(initial, checkpoint_signature, no_trust);
    ASSERT_FALSE(untrusted);
    EXPECT_EQ(untrusted.error().code, liquid::errc::unauthorized);
    EXPECT_NE(untrusted.error().message.find("untrusted TSA"), std::string::npos);

    auto trust = fixture_trust();
    auto changed = initial;
    changed.back() ^= std::byte{1};
    const auto changed_result =
        liquid::transparency::evidence_record::verify(changed, checkpoint_signature, trust);
    ASSERT_FALSE(changed_result);
    EXPECT_NE(changed_result.error().message.find("initial timestamp token"), std::string::npos);

    auto broken = renewed;
    broken.back() ^= std::byte{1};
    const auto broken_result =
        liquid::transparency::evidence_record::verify(broken, checkpoint_signature, trust);
    ASSERT_FALSE(broken_result);
    EXPECT_NE(broken_result.error().message.find("broken predecessor link"), std::string::npos);
}

TEST(TransparencyTimestamp, RejectsWrongImprintAndNamesUnsupportedDigest) {
    const auto record = read_bytes(fixture("lake-initial.ers"));
    const auto checkpoint_signature = read_bytes(fixture("checkpoint-signature.bin"));
    auto trust = fixture_trust();

    auto wrong_signature = checkpoint_signature;
    wrong_signature.front() ^= std::byte{1};
    const auto wrong_imprint =
        liquid::transparency::evidence_record::verify(record, wrong_signature, trust);
    ASSERT_FALSE(wrong_imprint);
    EXPECT_NE(wrong_imprint.error().message.find("message imprint"), std::string::npos);

    auto unsupported_record = record;
    const std::vector<std::byte> sha256_oid{
        std::byte{0x60},
        std::byte{0x86},
        std::byte{0x48},
        std::byte{0x01},
        std::byte{0x65},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x02},
        std::byte{0x01},
    };
    const auto occurrence = std::search(
        unsupported_record.begin(), unsupported_record.end(), sha256_oid.begin(), sha256_oid.end());
    ASSERT_NE(occurrence, unsupported_record.end());
    *(occurrence + 8) = std::byte{0x02};
    const auto unsupported = liquid::transparency::evidence_record::verify(
        unsupported_record, checkpoint_signature, trust);
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, liquid::errc::unsupported);
    EXPECT_NE(unsupported.error().message.find("2.16.840.1.101.3.4.2.2"), std::string::npos);
    EXPECT_NE(unsupported.error().message.find("SHA-256 required"), std::string::npos);
}

TEST(TransparencyTimestamp, RejectsCertificateWithoutTimestampPurpose) {
    auto record = read_bytes(fixture("lake-initial.ers"));
    const auto checkpoint_signature = read_bytes(fixture("checkpoint-signature.bin"));
    auto trust = fixture_trust();
    const std::vector<std::byte> timestamp_eku{
        std::byte{0x2b},
        std::byte{0x06},
        std::byte{0x01},
        std::byte{0x05},
        std::byte{0x05},
        std::byte{0x07},
        std::byte{0x03},
        std::byte{0x08},
    };
    const auto occurrence =
        std::search(record.begin(), record.end(), timestamp_eku.begin(), timestamp_eku.end());
    ASSERT_NE(occurrence, record.end());
    *(occurrence + 7) = std::byte{0x09};
    const auto rejected =
        liquid::transparency::evidence_record::verify(record, checkpoint_signature, trust);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, liquid::errc::unauthorized);
    EXPECT_NE(rejected.error().message.find("id-kp-timeStamping"), std::string::npos);
}

TEST(TransparencySignature, Sha384MatchesStandardVector) {
    constexpr std::string_view input = "abc";
    const auto encoded = std::as_bytes(std::span(input));
    const auto digest = liquid::crypto::digest(liquid::crypto::digest_kind::sha384, encoded);
    ASSERT_TRUE(digest) << digest.error().message;
    EXPECT_EQ(hex(*digest),
              "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
              "8086072ba1e7cc2358baeca134c825a7");
}

TEST(TransparencySignature, VerifiesRsaPkcs1AndPss) {
    const auto key = generate_key(EVP_PKEY_RSA);
    ASSERT_TRUE(key);
    const auto public_key = rsa_public_key(key.get());
    ASSERT_TRUE(public_key);
    constexpr std::string_view input = "liquid timestamp signature interoperability";
    const auto message = std::as_bytes(std::span(input));

    const auto pkcs1_signature = sign(key.get(), EVP_sha256(), message);
    ASSERT_FALSE(pkcs1_signature.empty());
    const auto pkcs1 =
        liquid::crypto::verify_signature(*public_key,
                                         liquid::crypto::signature_parameters{
                                             .kind = liquid::crypto::signature_kind::rsa_pkcs1,
                                             .digest = liquid::crypto::digest_kind::sha256,
                                         },
                                         message,
                                         pkcs1_signature);
    EXPECT_TRUE(pkcs1) << pkcs1.error().message;

    const auto pss_signature = sign(key.get(), EVP_sha384(), message, true);
    ASSERT_FALSE(pss_signature.empty());
    const auto pss =
        liquid::crypto::verify_signature(*public_key,
                                         liquid::crypto::signature_parameters{
                                             .kind = liquid::crypto::signature_kind::rsa_pss,
                                             .digest = liquid::crypto::digest_kind::sha384,
                                             .pss_salt_length = 48,
                                         },
                                         message,
                                         pss_signature);
    EXPECT_TRUE(pss) << pss.error().message;

    auto changed = pss_signature;
    changed.back() ^= std::byte{1};
    EXPECT_FALSE(
        liquid::crypto::verify_signature(*public_key,
                                         liquid::crypto::signature_parameters{
                                             .kind = liquid::crypto::signature_kind::rsa_pss,
                                             .digest = liquid::crypto::digest_kind::sha384,
                                             .pss_salt_length = 48,
                                         },
                                         message,
                                         changed));
}

TEST(TransparencySignature, VerifiesP384Sha384) {
    const auto key = generate_key(EVP_PKEY_EC, NID_secp384r1);
    ASSERT_TRUE(key);
    const auto public_key = ec_public_key(key.get(), liquid::crypto::curve_kind::p384, 48);
    ASSERT_TRUE(public_key);
    constexpr std::string_view input = "liquid P-384 timestamp signature interoperability";
    const auto message = std::as_bytes(std::span(input));
    const auto signature = sign(key.get(), EVP_sha384(), message);
    ASSERT_FALSE(signature.empty());
    const auto verified =
        liquid::crypto::verify_signature(*public_key,
                                         liquid::crypto::signature_parameters{
                                             .kind = liquid::crypto::signature_kind::ecdsa,
                                             .digest = liquid::crypto::digest_kind::sha384,
                                         },
                                         message,
                                         signature);
    EXPECT_TRUE(verified) << verified.error().message;

    auto changed = signature;
    changed.back() ^= std::byte{1};
    EXPECT_FALSE(liquid::crypto::verify_signature(*public_key,
                                                  liquid::crypto::signature_parameters{
                                                      .kind = liquid::crypto::signature_kind::ecdsa,
                                                      .digest = liquid::crypto::digest_kind::sha384,
                                                  },
                                                  message,
                                                  changed));
}
