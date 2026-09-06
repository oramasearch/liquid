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
    return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                         "project target must be oci, deb, rpm, apk, or helm",
                                         std::string{value}});
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
        return usage("project <oci|deb|rpm|apk|helm> <result|install-tree> --name N --version V");
    }
    auto selected = target(argv[0]);
    auto source_id = liquid::parse_digest(argv[1]);
    if (!selected || !source_id) {
        return fail(selected ? source_id.error() : selected.error());
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
            argument == "--out" || argument == "--depends" || argument == "--against" ||
            argument == "--replaces" || argument == "--service") {
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
            } else if (argument == "--against") {
                request.options.against = std::move(*parsed);
            } else if (argument == "--replaces") {
                request.metadata.replaces.push_back(std::move(*parsed));
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
    auto result_page =
        liquid::runtime::block_on(repository->results({.id = liquid::result_id{*source_id}}));
    if (result_page && !result_page->records.empty()) {
        request.sources.push_back({liquid::result_id{*source_id}});
    } else {
        request.sources.push_back({*source_id});
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
    for (const auto& attestation : report->attestations) {
        std::println("attestation: {}", liquid::to_string(attestation));
    }
    return 0;
}

} // namespace liquid_cli
