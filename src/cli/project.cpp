#include <charconv>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/projection.hpp"
#include "liquid/repository.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto target(std::string_view value) -> liquid::result<liquid::projection_target> {
    if (value == "oci") {
        return liquid::projection_target::oci_image;
    }
    if (value == "deb") {
        return liquid::projection_target::deb;
    }
    if (value == "rpm") {
        return liquid::projection_target::rpm;
    }
    if (value == "apk") {
        return liquid::projection_target::apk;
    }
    if (value == "helm") {
        return liquid::projection_target::helm_chart;
    }
    if (value == "macos-pkg") {
        return liquid::projection_target::macos_pkg;
    }
    if (value == "macos-dmg") {
        return liquid::projection_target::macos_dmg;
    }
    if (value == "msix") {
        return liquid::projection_target::msix;
    }
    if (value == "winget") {
        return liquid::projection_target::winget_manifest;
    }
    return std::unexpected(liquid::error{
        liquid::errc::invalid_argument, "unknown project target", std::string{value}});
}

auto service(std::string_view value) -> liquid::result<liquid::projection_service> {
    const auto comma = value.find(',');
    const auto identity = value.substr(0, comma);
    const auto equals = identity.find('=');
    const auto at = identity.rfind('@');
    if (equals == std::string_view::npos || at == std::string_view::npos || equals >= at) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument,
            "service must be name=repository@sha256:digest[,port=N][,env=K=V][,volume=P]",
            std::string{value}});
    }
    auto digest = liquid::parse_digest(identity.substr(at + 1));
    if (!digest) {
        return std::unexpected(digest.error());
    }
    auto output = liquid::projection_service{
        .name = std::string{identity.substr(0, equals)},
        .image_repository = std::string{identity.substr(equals + 1, at - equals - 1)},
        .image = *digest};
    auto offset = comma;
    while (offset != std::string_view::npos) {
        const auto begin = offset + 1;
        const auto next = value.find(',', begin);
        const auto field = value.substr(begin, next - begin);
        if (field.starts_with("port=")) {
            auto number = std::uint32_t{};
            const auto text = field.substr(5);
            const auto [end, code] =
                std::from_chars(text.data(), text.data() + text.size(), number);
            if (code != std::errc{} || end != text.data() + text.size() || number == 0 ||
                number > 65535) {
                return std::unexpected(liquid::error{
                    liquid::errc::invalid_argument, "service port is invalid", std::string{field}});
            }
            output.ports.push_back(static_cast<std::uint16_t>(number));
        } else if (field.starts_with("env=")) {
            const auto assignment = field.substr(4);
            const auto separator = assignment.find('=');
            if (separator == std::string_view::npos) {
                return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                                     "service environment needs NAME=VALUE",
                                                     std::string{field}});
            }
            output.env[std::string{assignment.substr(0, separator)}] =
                std::string{assignment.substr(separator + 1)};
        } else if (field.starts_with("volume=")) {
            output.volumes.push_back(std::string{field.substr(7)});
        } else {
            return std::unexpected(liquid::error{
                liquid::errc::invalid_argument, "unknown service field", std::string{field}});
        }
        offset = next;
    }
    return output;
}

} // namespace

auto project(int argc, char** argv) -> int {
    if (argc < 2) {
        return usage("project <target> <result|install-tree|installer> --name N --version V");
    }
    auto selected = target(argv[0]);
    if (!selected) {
        return fail(selected.error());
    }
    auto source_id = liquid::result<liquid::digest>{};
    if (*selected != liquid::projection_target::winget_manifest) {
        source_id = liquid::parse_digest(argv[1]);
        if (!source_id) {
            return fail(source_id.error());
        }
    }
    auto request = liquid::projection_request{.target = *selected};
    auto json = false;
    for (auto index = 2; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        const auto value = [&](std::string_view option) -> liquid::result<std::string> {
            if (++index == argc) {
                return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                                     std::string{option} + " requires a value",
                                                     "project"});
            }
            return std::string{argv[index]};
        };
        if (argument == "--name" || argument == "--version" || argument == "--arch" ||
            argument == "--out" || argument == "--depends" || argument == "--service" ||
            argument == "--identifier" || argument == "--publisher" ||
            argument == "--display-name" || argument == "--executable" ||
            argument == "--entitlements" || argument == "--installer-url") {
            auto parsed = value(argument);
            if (!parsed) {
                return fail(parsed.error());
            }
            if (argument == "--name") {
                request.metadata.name = std::move(*parsed);
            } else if (argument == "--version") {
                request.metadata.version = std::move(*parsed);
            } else if (argument == "--arch") {
                request.metadata.architecture = std::move(*parsed);
            } else if (argument == "--out") {
                request.options.output_directory = std::move(*parsed);
            } else if (argument == "--depends") {
                request.metadata.declared_dependencies.push_back(std::move(*parsed));
            } else if (argument == "--identifier") {
                request.metadata.identifier = std::move(*parsed);
            } else if (argument == "--publisher") {
                request.metadata.publisher = std::move(*parsed);
            } else if (argument == "--display-name") {
                request.metadata.display_name = std::move(*parsed);
            } else if (argument == "--executable") {
                request.metadata.executable = std::move(*parsed);
            } else if (argument == "--entitlements") {
                request.options.entitlements = std::move(*parsed);
            } else if (argument == "--installer-url") {
                request.options.installer_url = std::move(*parsed);
            } else {
                auto parsed_service = service(*parsed);
                if (!parsed_service) {
                    return fail(parsed_service.error());
                }
                request.services.push_back(std::move(*parsed_service));
            }
            continue;
        }
        if (argument == "--compat-gzip") {
            request.options.compat_gzip = true;
            continue;
        }
        if (argument == "--allow-abi-break") {
            request.options.allow_abi_break = true;
            continue;
        }
        if (argument == "--json") {
            json = true;
            continue;
        }
        return usage("project received an unknown option");
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open_build(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    if (*selected == liquid::projection_target::winget_manifest) {
        request.sources.push_back({std::filesystem::path{argv[1]}});
    } else {
        auto result_page =
            liquid::runtime::block_on(repository->results({.id = liquid::result_id{*source_id}}));
        if (result_page && !result_page->records.empty()) {
            request.sources.push_back({liquid::result_id{*source_id}});
        } else {
            request.sources.push_back({*source_id});
        }
    }
    auto report = liquid::runtime::block_on(liquid::project(*repository, std::move(request)));
    if (!report) {
        return fail(report.error());
    }
    if (json) {
        auto encoded = liquid::projection_report_json(*report);
        if (!encoded) {
            return fail(encoded.error());
        }
        std::println("{}", *encoded);
        return 0;
    }
    for (const auto& artifact : report->artifacts) {
        std::println("artifact: {} {} {}",
                     artifact.path.string(),
                     liquid::to_string(artifact.digest),
                     artifact.media_type);
    }
    for (const auto& request_value : report->signing_requests) {
        std::println("signing-request: {} {}",
                     request_value.path.string(),
                     liquid::to_string(request_value.unsigned_digest));
    }
    for (const auto& attestation : report->attestations) {
        std::println("attestation: {}", liquid::to_string(attestation));
    }
    return 0;
}

} // namespace liquid_cli
