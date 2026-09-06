#include "liquid/transparency.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

#include "crypto/cms_internal.hpp"
#include "crypto/signature.hpp"
#include "liquid/asn1.hpp"
#include "liquid/cms.hpp"

namespace liquid::transparency {
namespace {

using asn1::element;
using asn1::reader;
using asn1::tag;
using asn1::tag_class;

constexpr tag sequence{tag_class::universal, 16, true};
constexpr tag octet_string{tag_class::universal, 4, false};

struct tst_info {
    std::chrono::sys_seconds generated_at;
    std::string policy;
    std::string nonce;
    std::vector<std::byte> imprint;
};

struct archive_value {
    std::vector<std::byte> encoded;
    std::vector<std::byte> token;
};

auto corrupt(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "RFC 3161/RFC 4998"};
}

auto unsupported(std::string message) -> error {
    return error{errc::unsupported, std::move(message), "RFC 3161/RFC 4998"};
}

auto copy(std::span<const std::byte> input) -> std::vector<std::byte> {
    return {input.begin(), input.end()};
}

auto next_required(reader& input, std::string_view name) -> result<element> {
    auto value = input.next();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(corrupt("missing " + std::string(name)));
    }
    return **value;
}

auto oid(const element& value) -> result<std::string> {
    auto output = asn1::object_identifier(value);
    if (!output) {
        return std::unexpected(corrupt(output.error().message));
    }
    return output;
}

auto integer_text(const element& value) -> result<std::string> {
    auto bytes = asn1::unsigned_integer(value);
    if (!bytes || bytes->empty()) {
        return std::unexpected(corrupt("nonce is not a non-negative DER INTEGER"));
    }
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string output;
    output.reserve(bytes->size() * 2);
    for (const std::byte value_byte : *bytes) {
        const auto number = std::to_integer<std::uint8_t>(value_byte);
        output.push_back(alphabet[number >> 4]);
        output.push_back(alphabet[number & 0x0f]);
    }
    return output;
}

auto parse_decimal(std::string_view text, std::size_t offset, std::size_t count)
    -> std::optional<unsigned int> {
    if (offset + count > text.size()) {
        return std::nullopt;
    }
    unsigned int output = 0;
    for (std::size_t index = offset; index < offset + count; ++index) {
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
            return std::nullopt;
        }
        output = output * 10 + static_cast<unsigned int>(text[index] - '0');
    }
    return output;
}

auto generalized_time(const element& value) -> result<std::chrono::sys_seconds> {
    if (value.identifier != tag{tag_class::universal, 24, false}) {
        return std::unexpected(corrupt("TSTInfo genTime is not GeneralizedTime"));
    }
    const std::string_view text(reinterpret_cast<const char*>(value.content.data()),
                                value.content.size());
    if (text.size() < 15 || text.back() != 'Z') {
        return std::unexpected(corrupt("TSTInfo genTime is not UTC with seconds"));
    }
    std::size_t suffix = 14;
    if (text.size() > 15) {
        if (text[14] != '.' || text.size() == 16 || text[text.size() - 2] == '0') {
            return std::unexpected(corrupt("TSTInfo genTime fraction is not canonical DER"));
        }
        for (std::size_t index = 15; index + 1 < text.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
                return std::unexpected(corrupt("TSTInfo genTime fraction is invalid"));
            }
        }
        suffix = text.size() - 1;
    }
    if (suffix != 14 && text[14] != '.') {
        return std::unexpected(corrupt("TSTInfo genTime is invalid"));
    }
    const auto year = parse_decimal(text, 0, 4);
    const auto month = parse_decimal(text, 4, 2);
    const auto day = parse_decimal(text, 6, 2);
    const auto hour = parse_decimal(text, 8, 2);
    const auto minute = parse_decimal(text, 10, 2);
    const auto second = parse_decimal(text, 12, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::unexpected(corrupt("TSTInfo genTime is invalid"));
    }
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(*year)},
                                           std::chrono::month{*month},
                                           std::chrono::day{*day}};
    if (!date.ok()) {
        return std::unexpected(corrupt("TSTInfo genTime date is invalid"));
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

auto sha256_algorithm(const element& value, std::string_view location) -> result<void> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt(std::string(location) + " algorithm is invalid"));
    }
    reader fields(value.content);
    auto identifier = next_required(fields, "digest object identifier");
    if (!identifier) {
        return std::unexpected(identifier.error());
    }
    auto identifier_text = oid(*identifier);
    if (!identifier_text) {
        return std::unexpected(identifier_text.error());
    }
    auto parameters = fields.next();
    if (!parameters || !fields.empty()) {
        return std::unexpected(corrupt(std::string(location) + " algorithm fields are invalid"));
    }
    if (*identifier_text != "2.16.840.1.101.3.4.2.1") {
        return std::unexpected(unsupported(std::string(location) +
                                           " uses unsupported digest algorithm " +
                                           *identifier_text + " (SHA-256 required)"));
    }
    if (*parameters && ((*parameters)->identifier != tag{tag_class::universal, 5, false} ||
                        !(*parameters)->content.empty())) {
        return std::unexpected(
            corrupt(std::string(location) + " algorithm parameters are invalid"));
    }
    return {};
}

auto parse_message_imprint(const element& value) -> result<std::vector<std::byte>> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("TSTInfo messageImprint is invalid"));
    }
    reader fields(value.content);
    auto algorithm = next_required(fields, "message imprint algorithm");
    auto digest = next_required(fields, "message imprint digest");
    if (!algorithm || !digest || !fields.empty() || digest->identifier != octet_string) {
        return std::unexpected(corrupt("TSTInfo messageImprint fields are invalid"));
    }
    auto supported = sha256_algorithm(*algorithm, "TSTInfo message imprint");
    if (!supported) {
        return std::unexpected(supported.error());
    }
    if (digest->content.size() != 32) {
        return std::unexpected(corrupt("TSTInfo SHA-256 message imprint has the wrong size"));
    }
    return copy(digest->content);
}

auto parse_tst_info(std::span<const std::byte> encoded) -> result<tst_info> {
    reader input(encoded);
    auto sequence_value = input.single();
    if (!sequence_value || sequence_value->identifier != sequence) {
        return std::unexpected(corrupt("TSTInfo is not one DER sequence"));
    }
    reader fields(sequence_value->content);
    auto version = next_required(fields, "TSTInfo version");
    auto policy = next_required(fields, "TSTInfo policy");
    auto imprint = next_required(fields, "TSTInfo message imprint");
    auto serial = next_required(fields, "TSTInfo serial number");
    auto time = next_required(fields, "TSTInfo generation time");
    if (!version || !policy || !imprint || !serial || !time) {
        return std::unexpected(corrupt("TSTInfo fields are incomplete"));
    }
    auto version_bytes = asn1::unsigned_integer(*version);
    auto serial_bytes = asn1::unsigned_integer(*serial);
    if (!version_bytes || version_bytes->size() != 1 || version_bytes->front() != std::byte{1} ||
        !serial_bytes || serial_bytes->empty()) {
        return std::unexpected(corrupt("TSTInfo version or serial number is invalid"));
    }
    auto policy_text = oid(*policy);
    auto imprint_bytes = parse_message_imprint(*imprint);
    auto generated_at = generalized_time(*time);
    if (!policy_text || !imprint_bytes || !generated_at) {
        if (!policy_text) {
            return std::unexpected(policy_text.error());
        }
        return std::unexpected(imprint_bytes ? generated_at.error() : imprint_bytes.error());
    }

    bool accuracy_seen = false;
    bool ordering_seen = false;
    bool tsa_seen = false;
    bool extensions_seen = false;
    std::optional<std::string> nonce;
    while (!fields.empty()) {
        auto optional = next_required(fields, "TSTInfo optional field");
        if (!optional) {
            return std::unexpected(optional.error());
        }
        if (optional->identifier == sequence) {
            if (accuracy_seen || ordering_seen || nonce || tsa_seen || extensions_seen) {
                return std::unexpected(corrupt("TSTInfo accuracy is out of order"));
            }
            reader accuracy(optional->content);
            while (!accuracy.empty()) {
                auto component = next_required(accuracy, "TSTInfo accuracy component");
                if (!component) {
                    return std::unexpected(component.error());
                }
                if (component->identifier != tag{tag_class::universal, 2, false} &&
                    component->identifier != tag{tag_class::context_specific, 0, false} &&
                    component->identifier != tag{tag_class::context_specific, 1, false}) {
                    return std::unexpected(corrupt("TSTInfo accuracy component is invalid"));
                }
            }
            accuracy_seen = true;
        } else if (optional->identifier == tag{tag_class::universal, 1, false}) {
            if (ordering_seen || nonce || tsa_seen || extensions_seen ||
                optional->content.size() != 1 || optional->content.front() != std::byte{0xff}) {
                return std::unexpected(corrupt("TSTInfo ordering value is invalid"));
            }
            ordering_seen = true;
        } else if (optional->identifier == tag{tag_class::universal, 2, false}) {
            if (nonce || tsa_seen || extensions_seen) {
                return std::unexpected(corrupt("TSTInfo contains duplicate nonce fields"));
            }
            auto parsed_nonce = integer_text(*optional);
            if (!parsed_nonce) {
                return std::unexpected(parsed_nonce.error());
            }
            nonce = *parsed_nonce;
        } else if (optional->identifier == tag{tag_class::context_specific, 0, true}) {
            if (tsa_seen || extensions_seen) {
                return std::unexpected(corrupt("TSTInfo TSA name is out of order"));
            }
            reader tsa(optional->content);
            auto tsa_name = tsa.single();
            if (!tsa_name) {
                return std::unexpected(corrupt("TSTInfo TSA name is invalid"));
            }
            tsa_seen = true;
        } else if (optional->identifier == tag{tag_class::context_specific, 1, true}) {
            if (extensions_seen) {
                return std::unexpected(corrupt("TSTInfo contains duplicate extensions"));
            }
            extensions_seen = true;
        } else {
            return std::unexpected(corrupt("TSTInfo contains an unknown optional field"));
        }
    }
    if (!nonce) {
        return std::unexpected(corrupt("TSTInfo is not nonce-bound"));
    }
    return tst_info{
        .generated_at = *generated_at,
        .policy = *policy_text,
        .nonce = *nonce,
        .imprint = std::move(*imprint_bytes),
    };
}

auto verify_token(std::span<const std::byte> token,
                  std::span<const std::byte> imprint_value,
                  const x509::trust_anchor_set& trust) -> result<timestamp_verification> {
    auto content = cms::detail::signed_content(token);
    if (!content) {
        return std::unexpected(content.error());
    }
    auto info = parse_tst_info(*content);
    if (!info) {
        return std::unexpected(info.error());
    }
    auto expected_imprint = crypto::digest(crypto::digest_kind::sha256, imprint_value);
    if (!expected_imprint) {
        return std::unexpected(expected_imprint.error());
    }
    if (!std::ranges::equal(*expected_imprint, info->imprint)) {
        return std::unexpected(
            corrupt("RFC 3161 message imprint does not match the supplied value"));
    }
    auto signed_data = cms::signed_data::verify(token, trust, info->generated_at);
    if (!signed_data) {
        if (signed_data.error().code == errc::unauthorized) {
            return std::unexpected(error{
                errc::unauthorized, "untrusted TSA: " + signed_data.error().message, "RFC 3161"});
        }
        return std::unexpected(signed_data.error());
    }
    return timestamp_verification{
        .generated_at = info->generated_at,
        .policy = std::move(info->policy),
        .nonce = std::move(info->nonce),
        .digest_algorithm = "sha256",
        .signature_algorithm = std::move(signed_data->signature_algorithm),
        .signer = std::move(signed_data->signer),
        .certificate_path = std::move(signed_data->certificate_path),
    };
}

auto parse_archive_timestamp(const element& value) -> result<archive_value> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("ArchiveTimeStamp is not a sequence"));
    }
    reader fields(value.content);
    auto algorithm_wrapper = next_required(fields, "archive timestamp digest algorithm");
    auto token = next_required(fields, "archive timestamp token");
    if (!algorithm_wrapper || !token || !fields.empty() ||
        algorithm_wrapper->identifier != tag{tag_class::context_specific, 0, true} ||
        token->identifier != sequence) {
        return std::unexpected(corrupt("unsupported RFC 4998 ArchiveTimeStamp shape"));
    }
    reader wrapped(algorithm_wrapper->content);
    auto algorithm = wrapped.single();
    if (!algorithm) {
        return std::unexpected(corrupt("archive timestamp digest algorithm is invalid"));
    }
    auto supported = sha256_algorithm(*algorithm, "archive timestamp");
    if (!supported) {
        return std::unexpected(supported.error());
    }
    return archive_value{
        .encoded = copy(value.encoded),
        .token = copy(token->encoded),
    };
}

auto parse_evidence(std::span<const std::byte> encoded) -> result<std::vector<archive_value>> {
    reader input(encoded);
    auto record = input.single();
    if (!record || record->identifier != sequence) {
        return std::unexpected(corrupt("RFC 4998 EvidenceRecord is not one DER sequence"));
    }
    reader fields(record->content);
    auto version = next_required(fields, "evidence record version");
    auto algorithms = next_required(fields, "evidence record digest algorithms");
    auto archive_sequence = next_required(fields, "archive timestamp sequence");
    if (!version || !algorithms || !archive_sequence || !fields.empty() ||
        algorithms->identifier != sequence || archive_sequence->identifier != sequence) {
        return std::unexpected(corrupt("unsupported RFC 4998 EvidenceRecord shape"));
    }
    auto version_bytes = asn1::unsigned_integer(*version);
    if (!version_bytes || version_bytes->size() != 1 || version_bytes->front() != std::byte{1}) {
        return std::unexpected(unsupported("unsupported RFC 4998 EvidenceRecord version"));
    }
    reader algorithm_values(algorithms->content);
    auto algorithm = algorithm_values.single();
    if (!algorithm) {
        return std::unexpected(corrupt("evidence record must contain one digest algorithm"));
    }
    auto supported = sha256_algorithm(*algorithm, "evidence record");
    if (!supported) {
        return std::unexpected(supported.error());
    }
    reader chains(archive_sequence->content);
    auto chain = chains.single();
    if (!chain || chain->identifier != sequence) {
        return std::unexpected(corrupt("evidence record must contain one archive timestamp chain"));
    }
    reader chain_fields(chain->content);
    auto timestamp_sequence = chain_fields.single();
    if (!timestamp_sequence || timestamp_sequence->identifier != sequence) {
        return std::unexpected(corrupt("archive timestamp chain is invalid"));
    }
    std::vector<archive_value> output;
    reader timestamps(timestamp_sequence->content);
    while (!timestamps.empty()) {
        auto timestamp = next_required(timestamps, "archive timestamp");
        if (!timestamp) {
            return std::unexpected(timestamp.error());
        }
        auto parsed = parse_archive_timestamp(*timestamp);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.push_back(std::move(*parsed));
    }
    if (output.empty()) {
        return std::unexpected(corrupt("evidence record has no archive timestamps"));
    }
    return output;
}

} // namespace

auto timestamp_token::verify(std::span<const std::byte> token,
                             std::span<const std::byte> imprint_value,
                             const x509::trust_anchor_set& trust)
    -> result<timestamp_verification> {
    return verify_token(token, imprint_value, trust);
}

auto evidence_record::verify(std::span<const std::byte> record,
                             std::span<const std::byte> checkpoint_signature_value,
                             const x509::trust_anchor_set& trust) -> result<evidence_verification> {
    auto archive_values = parse_evidence(record);
    if (!archive_values) {
        return std::unexpected(archive_values.error());
    }
    evidence_verification output{.digest_algorithm = "sha256"};
    std::span<const std::byte> covered = checkpoint_signature_value;
    for (std::size_t index = 0; index < archive_values->size(); ++index) {
        auto verified = verify_token((*archive_values)[index].token, covered, trust);
        if (!verified) {
            const std::string prefix = index == 0
                                           ? "initial timestamp token: "
                                           : "broken predecessor link at archive timestamp " +
                                                 std::to_string(index + 1) + ": ";
            error failure = verified.error();
            failure.message = prefix + failure.message;
            return std::unexpected(std::move(failure));
        }
        output.archive_timestamps.push_back(archive_timestamp_verification{
            .sequence = index + 1,
            .encoded = (*archive_values)[index].encoded,
            .timestamp = std::move(*verified),
        });
        covered = output.archive_timestamps.back().encoded;
    }
    return output;
}

} // namespace liquid::transparency
