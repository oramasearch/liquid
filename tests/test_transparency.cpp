#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
