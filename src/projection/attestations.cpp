// SPDX-License-Identifier: MIT

#include "projection/attestations.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include "build/provenance.hpp"
#include "formats/package/common.hpp"
#include "liquid/attestation.hpp"
#include "liquid/build.hpp"

namespace liquid::projection_detail {
namespace {

auto quote(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
        case '\"':
            output.push_back('\\');
            output.push_back(character);
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                constexpr auto digits = std::string_view{"0123456789abcdef"};
                const auto byte = static_cast<unsigned char>(character);
                output += "\\u00";
                output.push_back(digits[byte >> 4U]);
                output.push_back(digits[byte & 0x0fU]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('\"');
    return output;
}

auto base64(std::span<const std::byte> input) -> std::string {
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto output = std::string{};
    output.reserve((input.size() + 2) / 3 * 4);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 3) {
        const auto remaining = input.size() - offset;
        const auto value =
            std::uint32_t{std::to_integer<std::uint8_t>(input[offset])} << 16U |
            (remaining > 1 ? std::uint32_t{std::to_integer<std::uint8_t>(input[offset + 1])} << 8U
                           : 0U) |
            (remaining > 2 ? std::uint32_t{std::to_integer<std::uint8_t>(input[offset + 2])} : 0U);
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 63U] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 63U] : '=');
    }
    return output;
}

auto generic_dsse(const repository_attestation& record) -> result<std::string> {
    auto encoded = encode_attestation(record.value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return std::string{"{\"payload\":"} + quote(base64(*encoded)) +
           ",\"payloadType\":\"application/vnd.liquid.taap.attestation.v1+cbor\"," +
           "\"signatures\":[{\"keyid\":" + quote(record.value.issuer.taap_id) +
           ",\"sig\":" + quote(base64(record.value.signature)) +
           "}],\"liquidAttestationDigest\":" + quote(to_string(record.id)) + "}";
}

auto sbom_dsse(const repository_attestation& record, std::string_view document)
    -> result<std::string> {
    auto encoded = encode_attestation(record.value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto payload = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(document.data()), document.size()};
    return std::string{"{\"payload\":"} + quote(base64(payload)) +
           ",\"payloadType\":\"application/json\",\"signatures\":[{\"keyid\":" +
           quote(record.value.issuer.taap_id) +
           ",\"sig\":" + quote(base64(record.value.signature)) +
           "}],\"liquidAttestation\":" + quote(base64(*encoded)) +
           ",\"liquidAttestationDigest\":" + quote(to_string(record.id)) + "}";
}

auto append_unique(std::vector<digest>& values, digest value) -> void {
    if (!std::ranges::contains(
            values, to_string(value), [](const digest& existing) { return to_string(existing); })) {
        values.push_back(std::move(value));
    }
}

} // namespace

runtime::task<sidecar_attachment> attach_sidecar(repository& source,
                                                 const prepared_projection& prepared,
                                                 projection_target target,
                                                 const std::filesystem::path& artifact,
                                                 digest subject) {
    auto projection = co_await store_projection_attestation(source, prepared, target, subject);
    if (!projection) {
        co_return std::unexpected(projection.error());
    }
    auto ids = prepared.source_attestations;
    append_unique(ids, *projection);
    auto provenance = std::map<std::string, std::string>{};
    auto sboms = std::map<std::string, std::string>{};
    for (const auto& result_id : prepared.source_results) {
        auto report = co_await build::verify_provenance(source, result_id);
        if (!report) {
            co_return std::unexpected(report.error());
        }
        if (!report->verified) {
            co_return std::unexpected(
                error{errc::unauthorized, "source provenance is invalid", report->reason});
        }
        auto envelope = build::provenance_json(*report, true);
        if (!envelope) {
            co_return std::unexpected(envelope.error());
        }
        provenance.emplace(to_string(report->attestation), std::move(*envelope));
        auto options = record_page_options{};
        while (true) {
            auto page = co_await source.sboms(result_id, options);
            if (!page) {
                co_return std::unexpected(page.error());
            }
            for (const auto& sbom : page->records) {
                sboms.insert_or_assign(to_string(sbom.attestation), sbom.document);
            }
            if (page->done || !page->next) {
                break;
            }
            options.cursor = page->next;
        }
    }
    auto envelopes = std::vector<std::string>{};
    for (const auto& id : ids) {
        const auto exported = provenance.find(to_string(id));
        if (exported != provenance.end()) {
            envelopes.push_back(exported->second);
            continue;
        }
        auto page = co_await source.attestations({.id = id});
        if (!page) {
            co_return std::unexpected(page.error());
        }
        if (page->records.empty()) {
            co_return std::unexpected(
                error{errc::corrupt, "attached attestation is absent", to_string(id)});
        }
        const auto sbom = sboms.find(to_string(id));
        auto encoded = co_await runtime::offload([record = page->records.front(),
                                                  document = sbom == sboms.end()
                                                                 ? std::optional<std::string>{}
                                                                 : std::optional{sbom->second}] {
            return document ? sbom_dsse(record, *document) : generic_dsse(record);
        });
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        if (!*encoded) {
            co_return std::unexpected(encoded->error());
        }
        envelopes.push_back(std::move(**encoded));
    }
    auto json = std::string{"{\"subject\":"} + quote(to_string(subject)) + ",\"attestations\":[";
    for (auto index = std::size_t{}; index < envelopes.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        json += envelopes[index];
    }
    json += "]}";
    auto bytes = formats::package::text_bytes(json);
    auto sidecar_digest = formats::package::sha256(bytes);
    if (!sidecar_digest) {
        co_return std::unexpected(sidecar_digest.error());
    }
    auto path = std::filesystem::path{artifact.string() + ".attestations.json"};
    auto stored = co_await runtime::offload(
        [path, bytes = std::move(bytes)] { return write_file(path, bytes); });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    co_return sidecar_attachment{*projection,
                                 {path, *sidecar_digest, "application/vnd.dsse.envelope.v1+json"}};
}

} // namespace liquid::projection_detail
