#include "projection/oci.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "formats/deflate/deflate.hpp"
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
        case '\n':
            output += "\\n";
            break;
        default:
            output.push_back(character);
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

auto descriptor_json(const oci_descriptor& value) -> std::string {
    auto output = std::string{"{\"mediaType\":"} + quote(value.media_type) +
                  ",\"digest\":" + quote(to_string(value.content)) +
                  ",\"size\":" + std::to_string(value.size);
    if (!value.artifact_type.empty()) {
        output += ",\"artifactType\":" + quote(value.artifact_type);
    }
    if (!value.annotations.empty()) {
        output += ",\"annotations\":{";
        auto first = true;
        for (const auto& [name, text] : value.annotations) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            output += quote(name) + ":" + quote(text);
        }
        output.push_back('}');
    }
    output.push_back('}');
    return output;
}

auto string_array(std::span<const std::string> values) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += quote(values[index]);
    }
    output.push_back(']');
    return output;
}

auto generic_dsse(const repository_attestation& record) -> result<std::vector<std::byte>> {
    auto encoded = encode_attestation(record.value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto json = std::string{"{\"payload\":"} + quote(base64(*encoded)) +
                ",\"payloadType\":\"application/vnd.liquid.taap.attestation.v1+cbor\"," +
                "\"signatures\":[{\"keyid\":" + quote(record.value.issuer.taap_id) +
                ",\"sig\":" + quote(base64(record.value.signature)) +
                "}],\"liquidAttestationDigest\":" + quote(to_string(record.id)) + "}";
    return formats::package::text_bytes(json);
}

auto sbom_dsse(const repository_attestation& record, std::string_view document)
    -> result<std::vector<std::byte>> {
    auto encoded = encode_attestation(record.value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto payload = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(document.data()), document.size()};
    auto json = std::string{"{\"payload\":"} + quote(base64(payload)) +
                ",\"payloadType\":\"application/json\",\"signatures\":[{\"keyid\":" +
                quote(record.value.issuer.taap_id) +
                ",\"sig\":" + quote(base64(record.value.signature)) +
                "}],\"liquidAttestation\":" + quote(base64(*encoded)) +
                ",\"liquidAttestationDigest\":" + quote(to_string(record.id)) + "}";
    return formats::package::text_bytes(json);
}

auto profile_name(const attestation& statement) -> std::string {
    const auto slash = statement.profile.find('/');
    return slash == std::string::npos ? statement.profile : statement.profile.substr(0, slash);
}

} // namespace

result<oci_descriptor>
oci_blob(std::filesystem::path root, std::span<const std::byte> bytes, std::string media_type) {
    auto id = formats::package::sha256(bytes);
    if (!id) {
        return std::unexpected(id.error());
    }
    auto stored =
        write_file(root / "blobs" / "sha256" / formats::package::hex_bytes(id->bytes), bytes);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    return oci_descriptor{std::move(media_type), *id, bytes.size()};
}

result<oci_descriptor> oci_manifest(std::filesystem::path root,
                                    const oci_descriptor& config,
                                    std::span<const oci_descriptor> layers,
                                    std::optional<oci_descriptor> subject,
                                    std::string artifact_type,
                                    std::map<std::string, std::string> annotations) {
    auto json = std::string{"{\"schemaVersion\":2,\"mediaType\":"} +
                quote("application/vnd.oci.image.manifest.v1+json");
    if (!artifact_type.empty()) {
        json += ",\"artifactType\":" + quote(artifact_type);
    }
    if (subject) {
        json += ",\"subject\":" + descriptor_json(*subject);
    }
    json += ",\"config\":" + descriptor_json(config) + ",\"layers\":[";
    for (auto index = std::size_t{}; index < layers.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        json += descriptor_json(layers[index]);
    }
    json.push_back(']');
    if (!annotations.empty()) {
        json += ",\"annotations\":{";
        auto first = true;
        for (const auto& [name, value] : annotations) {
            if (!first) {
                json.push_back(',');
            }
            first = false;
            json += quote(name) + ":" + quote(value);
        }
        json.push_back('}');
    }
    json.push_back('}');
    auto descriptor = oci_blob(
        root, formats::package::text_bytes(json), "application/vnd.oci.image.manifest.v1+json");
    if (descriptor) {
        descriptor->artifact_type = std::move(artifact_type);
    }
    return descriptor;
}

runtime::task<std::vector<oci_descriptor>> attach_attestations(repository& source,
                                                               const prepared_projection& prepared,
                                                               std::filesystem::path root,
                                                               const oci_descriptor& subject,
                                                               digest projection_attestation) {
    auto ids = prepared.source_attestations;
    ids.push_back(projection_attestation);
    auto provenance = std::map<std::string, std::vector<std::byte>>{};
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
        provenance.emplace(to_string(report->attestation), formats::package::text_bytes(*envelope));
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
    auto empty = co_await runtime::offload([root] {
        return oci_blob(
            root, formats::package::text_bytes("{}"), "application/vnd.oci.empty.v1+json");
    });
    if (!empty) {
        co_return std::unexpected(empty.error());
    }
    if (!*empty) {
        co_return std::unexpected(empty->error());
    }
    auto output = std::vector<oci_descriptor>{};
    for (const auto& id : ids) {
        auto page = co_await source.attestations({.id = id});
        if (!page) {
            co_return std::unexpected(page.error());
        }
        if (page->records.empty()) {
            co_return std::unexpected(
                error{errc::corrupt, "attached attestation is absent", to_string(id)});
        }
        auto envelope = std::vector<std::byte>{};
        const auto exported = provenance.find(to_string(id));
        if (exported != provenance.end()) {
            envelope = exported->second;
        } else {
            const auto sbom = sboms.find(to_string(id));
            auto encoded = co_await runtime::offload(
                [record = page->records.front(),
                 document = sbom == sboms.end() ? std::optional<std::string>{}
                                                : std::optional{sbom->second}] {
                    return document ? sbom_dsse(record, *document) : generic_dsse(record);
                });
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            if (!*encoded) {
                co_return std::unexpected(encoded->error());
            }
            envelope = std::move(**encoded);
        }
        auto manifest = co_await runtime::offload(
            [root, envelope = std::move(envelope), empty = **empty, subject]() mutable {
                auto layer = oci_blob(root, envelope, "application/vnd.dsse.envelope.v1+json");
                if (!layer) {
                    return result<oci_descriptor>{std::unexpected(layer.error())};
                }
                return oci_manifest(root,
                                    empty,
                                    std::span{&*layer, 1},
                                    subject,
                                    "application/vnd.dsse.envelope.v1+json");
            });
        if (!manifest) {
            co_return std::unexpected(manifest.error());
        }
        if (!*manifest) {
            co_return std::unexpected(manifest->error());
        }
        (**manifest).annotations["org.opencontainers.image.ref.name"] =
            "sha256-" + formats::package::hex_bytes(subject.content.bytes) + "." +
            profile_name(page->records.front().value);
        output.push_back(std::move(**manifest));
    }
    co_return output;
}

result<void> write_oci_index(std::filesystem::path root,
                             std::span<const oci_descriptor> primary,
                             std::span<const oci_descriptor> referrers) {
    auto layout = write_file(root / "oci-layout",
                             formats::package::text_bytes("{\"imageLayoutVersion\":\"1.0.0\"}"));
    if (!layout) {
        return std::unexpected(layout.error());
    }
    auto json = std::string{"{\"schemaVersion\":2,\"mediaType\":"} +
                quote("application/vnd.oci.image.index.v1+json") + ",\"manifests\":[";
    auto first = true;
    for (const auto* values : {&primary, &referrers}) {
        for (const auto& value : *values) {
            if (!first) {
                json.push_back(',');
            }
            first = false;
            json += descriptor_json(value);
        }
    }
    json += "]}";
    return write_file(root / "index.json", formats::package::text_bytes(json));
}

runtime::task<projection_report> project_oci(repository& source, prepared_projection prepared) {
    auto built = co_await runtime::offload(
        [prepared]() mutable
        -> result<std::tuple<std::filesystem::path, oci_descriptor, std::vector<oci_descriptor>>> {
            const auto root = prepared.options.output_directory /
                              (prepared.metadata.name + "-" + prepared.metadata.version + ".oci");
            auto groups = std::array<std::vector<formats::package::file>, 3>{};
            for (auto file : prepared.files) {
                const auto path = file.path;
                file.path = "usr/" + file.path;
                const auto traced = prepared.stability_layers.find(path);
                const auto group = traced != prepared.stability_layers.end() ? traced->second
                                   : file.path.starts_with("usr/lib/")       ? 0U
                                   : file.path.starts_with("usr/bin/")       ? 2U
                                                                             : 1U;
                groups[group].push_back(std::move(file));
            }
            auto layers = std::vector<oci_descriptor>{};
            auto diff_ids = std::vector<std::string>{};
            for (const auto& group : groups) {
                if (group.empty()) {
                    continue;
                }
                auto archive = formats::package::tar(group, prepared.options.source_date_epoch);
                if (!archive) {
                    return std::unexpected(archive.error());
                }
                auto diff = formats::package::sha256(*archive);
                if (!diff) {
                    return std::unexpected(diff.error());
                }
                diff_ids.push_back(to_string(*diff));
                auto compressed = std::vector<std::byte>{};
                if (prepared.options.compat_gzip) {
                    compressed = formats::deflate::gzip(*archive);
                } else {
                    auto encoded = formats::package::zstd(*archive);
                    if (!encoded) {
                        return std::unexpected(encoded.error());
                    }
                    compressed = std::move(*encoded);
                }
                auto layer = oci_blob(root,
                                      compressed,
                                      prepared.options.compat_gzip
                                          ? "application/vnd.oci.image.layer.v1.tar+gzip"
                                          : "application/vnd.oci.image.layer.v1.tar+zstd");
                if (!layer) {
                    return std::unexpected(layer.error());
                }
                layers.push_back(std::move(*layer));
            }
            const auto created = std::format(
                "{:%FT%TZ}",
                std::chrono::sys_seconds{std::chrono::seconds{prepared.options.source_date_epoch}});
            auto config_json = std::string{"{\"created\":"} + quote(created) +
                               ",\"architecture\":" +
                               quote(prepared.metadata.architecture == "x86_64" ? "amd64"
                                     : prepared.metadata.architecture == "aarch64"
                                         ? "arm64"
                                         : prepared.metadata.architecture) +
                               ",\"os\":\"linux\",\"config\":{\"Entrypoint\":" +
                               string_array(prepared.options.oci_entrypoint) +
                               ",\"Cmd\":" + string_array(prepared.options.oci_command) +
                               "},\"rootfs\":{" + "\"type\":\"layers\",\"diff_ids\":[";
            for (auto index = std::size_t{}; index < diff_ids.size(); ++index) {
                if (index != 0) {
                    config_json.push_back(',');
                }
                config_json += quote(diff_ids[index]);
            }
            config_json += "]},\"history\":[";
            for (auto index = std::size_t{}; index < layers.size(); ++index) {
                if (index != 0) {
                    config_json.push_back(',');
                }
                config_json += "{\"created\":" + quote(created) +
                               ",\"created_by\":\"liquid projection stability layer " +
                               std::to_string(index) + "\"}";
            }
            config_json += "]}";
            auto config = oci_blob(root,
                                   formats::package::text_bytes(config_json),
                                   "application/vnd.oci.image.config.v1+json");
            if (!config) {
                return std::unexpected(config.error());
            }
            auto manifest = oci_manifest(root, *config, layers);
            if (!manifest) {
                return std::unexpected(manifest.error());
            }
            manifest->annotations["org.opencontainers.image.ref.name"] =
                prepared.metadata.name + ":" + prepared.metadata.version;
            return std::tuple{root, *manifest, std::move(layers)};
        });
    if (!built) {
        co_return std::unexpected(built.error());
    }
    if (!*built) {
        co_return std::unexpected(built->error());
    }
    auto& [root, manifest, layers] = **built;
    auto projection = co_await store_projection_attestation(
        source, prepared, projection_target::oci_image, manifest.content);
    if (!projection) {
        co_return std::unexpected(projection.error());
    }
    auto referrers = co_await attach_attestations(source, prepared, root, manifest, *projection);
    if (!referrers) {
        co_return std::unexpected(referrers.error());
    }
    auto indexed = co_await runtime::offload([root, manifest, referrers = *referrers] {
        return write_oci_index(root, std::span{&manifest, 1}, referrers);
    });
    if (!indexed) {
        co_return std::unexpected(indexed.error());
    }
    if (!*indexed) {
        co_return std::unexpected(indexed->error());
    }
    auto report = projection_report{.artifacts = {{root, manifest.content, manifest.media_type}},
                                    .attestations = prepared.source_attestations,
                                    .pinned = {{prepared.metadata.name, manifest.content}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    report.attestations.push_back(*projection);
    co_return report;
}

} // namespace liquid::projection_detail
