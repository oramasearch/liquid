#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
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
    if (value == "ostree") {
        return liquid::projection_target::ostree_commit;
    }
    if (value == "bootc") {
        return liquid::projection_target::bootc_image;
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
    if (value == "homebrew") {
        return liquid::projection_target::homebrew;
    }
    if (value == "pkgbuild") {
        return liquid::projection_target::pkgbuild;
    }
    if (value == "appimage") {
        return liquid::projection_target::appimage;
    }
    if (value == "flatpak") {
        return liquid::projection_target::flatpak;
    }
    if (value == "snap") {
        return liquid::projection_target::snap;
    }
    if (value == "dsc") {
        return liquid::projection_target::debian_source;
    }
    return std::unexpected(liquid::error{
        liquid::errc::invalid_argument, "unknown project target", std::string{value}});
}

auto same_digest(const liquid::digest& left, const liquid::digest& right) -> bool {
    return liquid::to_string(left) == liquid::to_string(right);
}

auto find_result(liquid::repository& repository, liquid::digest tree, bool output_tree)
    -> liquid::result<std::optional<liquid::result_id>> {
    auto page_options = liquid::record_page_options{};
    while (true) {
        auto page = liquid::runtime::block_on(repository.results({.page = page_options}));
        if (!page) {
            return std::unexpected(page.error());
        }
        for (const auto& record : page->records) {
            if (output_tree) {
                if (record.value.output_tree && same_digest(*record.value.output_tree, tree)) {
                    return std::optional{record.id};
                }
                continue;
            }
            auto action = liquid::runtime::block_on(repository.action(record.value.action));
            if (!action) {
                return std::unexpected(action.error());
            }
            if (same_digest(action->inputs, tree)) {
                return std::optional{record.id};
            }
        }
        if (page->done || !page->next) {
            break;
        }
        page_options.cursor = page->next;
    }
    return std::optional<liquid::result_id>{};
}

auto input_error(std::string_view kind, liquid::digest id) -> liquid::error {
    return {liquid::errc::invalid_argument,
            "project input is " + std::string{kind} +
                "; accepted inputs are a recorded result, change, or tree "
                "(installer paths are accepted only by winget)",
            liquid::to_string(id)};
}

// A missing object can belong to the build repository. A present but unsupported
// object must report its input error instead of suggesting build configuration.
auto resolve_input(liquid::repository& repository, liquid::digest candidate)
    -> liquid::result<std::optional<liquid::projection_source>> {
    auto tree = liquid::runtime::block_on(repository.tree(candidate));
    if (tree) {
        return liquid::projection_source{.value = candidate};
    }
    if (tree.error().code == liquid::errc::not_found) {
        return std::optional<liquid::projection_source>{};
    }
    if (tree.error().code != liquid::errc::corrupt) {
        return std::unexpected(tree.error());
    }
    auto changes = liquid::runtime::block_on(
        repository.log({.from = liquid::change_id{candidate}, .first_parent = true}));
    if (changes && !changes->empty()) {
        return liquid::projection_source{.value = changes->front().root,
                                         .change = liquid::change_id{candidate}};
    }
    auto result =
        liquid::runtime::block_on(liquid::build::result(repository, liquid::result_id{candidate}));
    if (result) {
        return liquid::projection_source{.value = liquid::result_id{candidate}};
    }
    auto blob = liquid::runtime::block_on(repository.blob(candidate));
    if (blob) {
        return std::unexpected(input_error("a blob", candidate));
    }
    auto action = liquid::runtime::block_on(repository.action(candidate));
    if (action) {
        return std::unexpected(input_error("an action", candidate));
    }
    return std::unexpected(input_error("an unsupported stored record", candidate));
}

struct projection_context {
    liquid::repository repository;
    liquid::projection_source source;
};

auto open_projection(liquid::digest candidate) -> liquid::result<projection_context> {
    const auto path = std::filesystem::current_path();
    auto repository = liquid::runtime::block_on(liquid::repository::open(path));
    if (repository) {
        auto source = resolve_input(*repository, candidate);
        if (!source) {
            return std::unexpected(source.error());
        }
        if (*source) {
            return projection_context{std::move(*repository), std::move(**source)};
        }
    }
    // Opening an uninitialized checkout reports an I/O error for its missing marker.
    // It can still have a build repository containing the requested result.
    auto build = liquid::runtime::block_on(liquid::repository::open_build(path));
    if (build) {
        auto source = resolve_input(*build, candidate);
        if (!source) {
            return std::unexpected(source.error());
        }
        if (*source) {
            return projection_context{std::move(*build), std::move(**source)};
        }
    } else if (build.error().code != liquid::errc::not_configured &&
               build.error().code != liquid::errc::not_found) {
        return std::unexpected(build.error());
    }
    return std::unexpected(
        liquid::error{liquid::errc::not_found,
                      "project input was not found in the current or build repository; "
                      "accepted inputs are a recorded result, change, or tree",
                      liquid::to_string(candidate)});
}

auto resolve_source_package(liquid::repository& repository, liquid::projection_request& request)
    -> liquid::result<void> {
    for (auto& source : request.sources) {
        if (std::holds_alternative<liquid::result_id>(source.value)) {
            continue;
        }
        const auto tree = std::get<liquid::digest>(source.value);
        auto evidence = find_result(repository, tree, !source.change.has_value());
        if (!evidence) {
            return std::unexpected(evidence.error());
        }
        if (source.change) {
            request.build_evidence = *evidence;
        } else if (*evidence) {
            source.value = **evidence;
        } else {
            return std::unexpected(
                liquid::error{liquid::errc::invalid_argument,
                              "dsc input is a bare tree without a producing result; "
                              "accepted inputs are a recorded change, result, or install tree with "
                              "a producing result",
                              liquid::to_string(tree)});
        }
    }
    return {};
}

auto ostree_mode(std::string_view value) -> liquid::result<liquid::ostree_repository_mode> {
    if (value == "archive-z2") {
        return liquid::ostree_repository_mode::archive_z2;
    }
    if (value == "bare-user") {
        return liquid::ostree_repository_mode::bare_user;
    }
    return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                         "ostree mode must be archive-z2 or bare-user",
                                         std::string{value}});
}

auto source_epoch() -> liquid::result<std::uint64_t> {
    const auto* value = std::getenv("SOURCE_DATE_EPOCH");
    if (value == nullptr) {
        return std::uint64_t{};
    }
    const auto text = std::string_view{value};
    auto output = std::uint64_t{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), output);
    if (code != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "SOURCE_DATE_EPOCH must be an unsigned integer",
                                             std::string{text}});
    }
    return output;
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
        return usage(
            "project <oci|ostree|bootc|deb|rpm|apk|helm|macos-pkg|macos-dmg|msix|winget|homebrew|"
            "pkgbuild|appimage|flatpak|snap|dsc> <result|change|tree|installer> [target options]");
    }
    auto selected = target(argv[0]);
    if (!selected) {
        return fail(selected.error());
    }
    auto sources = std::vector<liquid::projection_source>{};
    if (*selected != liquid::projection_target::winget_manifest) {
        auto source_id = liquid::parse_digest(argv[1]);
        if (!source_id) {
            return fail({liquid::errc::invalid_argument,
                         "project input is not a digest; accepted inputs are a recorded result, "
                         "change, or tree (installer paths are accepted only by winget)",
                         argv[1]});
        }
        sources.push_back({.value = *source_id});
    }
    auto epoch = source_epoch();
    if (!epoch) {
        return fail(epoch.error());
    }
    auto request =
        liquid::projection_request{.target = *selected, .options = {.source_date_epoch = *epoch}};
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
            argument == "--entitlements" || argument == "--installer-url" ||
            argument == "--against" || argument == "--replaces" || argument == "--ref" ||
            argument == "--overlay" || argument == "--mode" || argument == "--distribution") {
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
            } else if (argument == "--against") {
                request.options.against = std::move(*parsed);
            } else if (argument == "--replaces") {
                request.metadata.replaces.push_back(std::move(*parsed));
            } else if (argument == "--ref") {
                request.options.ostree_ref = std::move(*parsed);
            } else if (argument == "--overlay") {
                const auto equals = parsed->find('=');
                const auto digest_text = equals == std::string::npos
                                             ? std::string_view{*parsed}
                                             : std::string_view{*parsed}.substr(equals + 1);
                auto overlay = liquid::parse_digest(digest_text);
                if (!overlay) {
                    return fail(overlay.error());
                }
                sources.push_back({.value = *overlay,
                                   .path = equals == std::string::npos
                                               ? std::filesystem::path{}
                                               : std::filesystem::path{parsed->substr(0, equals)}});
            } else if (argument == "--mode") {
                auto mode = ostree_mode(*parsed);
                if (!mode) {
                    return fail(mode.error());
                }
                request.options.ostree_mode = *mode;
            } else if (argument == "--distribution") {
                request.options.distribution = std::move(*parsed);
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
    auto repository = liquid::repository{};
    if (*selected == liquid::projection_target::winget_manifest) {
        if (liquid::parse_digest(argv[1])) {
            return fail({liquid::errc::invalid_argument,
                         "winget input is a digest; accepted inputs are MSIX installer paths",
                         argv[1]});
        }
        auto opened =
            liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
        if (!opened) {
            opened = liquid::runtime::block_on(
                liquid::repository::open_build(std::filesystem::current_path()));
        }
        if (!opened) {
            if (opened.error().code == liquid::errc::not_configured) {
                return fail({liquid::errc::not_configured,
                             "winget requires a repository for projection attestations; "
                             "run liquid init . first",
                             argv[1]});
            }
            return fail(opened.error());
        }
        repository = std::move(*opened);
        request.sources.push_back({.path = std::filesystem::path{argv[1]}});
    } else {
        auto context = open_projection(std::get<liquid::digest>(sources.front().value));
        if (!context) {
            return fail(context.error());
        }
        repository = std::move(context->repository);
        request.sources.push_back(std::move(context->source));
        for (auto index = std::size_t{1}; index < sources.size(); ++index) {
            const auto candidate = std::get<liquid::digest>(sources[index].value);
            auto source = resolve_input(repository, candidate);
            if (!source) {
                return fail(source.error());
            }
            if (!*source || (**source).change ||
                !std::holds_alternative<liquid::digest>((**source).value)) {
                return fail(
                    {liquid::errc::invalid_argument,
                     "overlay input must be a tree in the same repository as the primary source",
                     liquid::to_string(candidate)});
            }
            (**source).path = std::move(sources[index].path);
            request.sources.push_back(std::move(**source));
        }
        if (*selected == liquid::projection_target::debian_source) {
            auto resolved = resolve_source_package(repository, request);
            if (!resolved) {
                return fail(resolved.error());
            }
        }
    }
    auto report = liquid::runtime::block_on(liquid::project(repository, std::move(request)));
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
    if (report->ostree_commit) {
        const auto checksum = liquid::to_string(*report->ostree_commit);
        std::println("commit: {}", checksum.substr(checksum.find(':') + 1));
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
    for (const auto& diagnostic : report->diagnostics) {
        std::println(stderr, "diagnostic: {}", diagnostic);
    }
    return 0;
}

} // namespace liquid_cli
