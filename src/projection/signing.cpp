#include "liquid/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>

#include "build/repository_access.hpp"
#include "formats/json.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "projection/platform_tools.hpp"
#include "projection/projection.hpp"

namespace liquid::projection_detail {
namespace {

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto field(const formats::json::value& value, std::string_view name)
    -> const formats::json::value* {
    const auto* object = std::get_if<formats::json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& item) { return std::string_view{item.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto text_field(const formats::json::value& value, std::string_view name) -> result<std::string> {
    const auto* selected = field(value, name);
    const auto* text = selected ? std::get_if<std::string>(&selected->data) : nullptr;
    if (!text) {
        return std::unexpected(
            failure(errc::corrupt, "signing request field is missing", std::string{name}));
    }
    return *text;
}

auto request_path(const std::filesystem::path& artifact) -> std::filesystem::path {
    return artifact.string() + ".liquid-signing-request.json";
}

auto attestation_path(const std::filesystem::path& artifact) -> std::filesystem::path {
    return artifact.string() + ".platform-signature.cbor";
}

auto read_request(const std::filesystem::path& artifact) -> result<platform_signing_request> {
    auto encoded = read_text_file(request_path(artifact));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto document = formats::json::parse(*encoded);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto schema = text_field(*document, "schema");
    auto platform = text_field(*document, "platform");
    auto artifact_value = text_field(*document, "artifact");
    auto unsigned_digest = text_field(*document, "unsigned_digest");
    auto identifier = text_field(*document, "identifier");
    auto executable = text_field(*document, "executable");
    auto entitlements = text_field(*document, "entitlements");
    if (!schema || !platform || !artifact_value || !unsigned_digest || !identifier || !executable ||
        !entitlements) {
        const auto* selected = !schema            ? &schema.error()
                               : !platform        ? &platform.error()
                               : !artifact_value  ? &artifact_value.error()
                               : !unsigned_digest ? &unsigned_digest.error()
                               : !identifier      ? &identifier.error()
                               : !executable      ? &executable.error()
                                                  : &entitlements.error();
        return std::unexpected(*selected);
    }
    if (*schema != "platform_signing_request/1") {
        return std::unexpected(
            failure(errc::corrupt, "unsupported signing request schema", *schema));
    }
    auto parsed_digest = parse_digest(*unsigned_digest);
    if (!parsed_digest) {
        return std::unexpected(parsed_digest.error());
    }
    auto selected_platform = platform_signing_system::macos;
    if (*platform == "windows") {
        selected_platform = platform_signing_system::windows;
    } else if (*platform != "macos") {
        return std::unexpected(
            failure(errc::corrupt, "unknown signing request platform", *platform));
    }
    if (std::filesystem::path{*artifact_value}.lexically_normal() != artifact.lexically_normal()) {
        return std::unexpected(
            failure(errc::conflict, "signing request names another artifact", *artifact_value));
    }
    return platform_signing_request{.path = request_path(artifact),
                                    .artifact = artifact,
                                    .unsigned_digest = *parsed_digest,
                                    .platform = selected_platform,
                                    .identifier = std::move(*identifier),
                                    .executable = std::move(*executable),
                                    .entitlements = std::move(*entitlements)};
}

auto require_tool(const tool_result& execution, std::string_view name) -> result<std::string> {
    auto diagnostic = execution.output;
    if (!execution.error_output.empty()) {
        diagnostic += execution.error_output;
    }
    while (!diagnostic.empty() && (diagnostic.back() == '\n' || diagnostic.back() == '\r')) {
        diagnostic.pop_back();
    }
    if (execution.exit != 0) {
        return std::unexpected(
            failure(errc::unauthorized, std::string{name} + " failed", std::move(diagnostic)));
    }
    return diagnostic;
}

auto signed_path(const std::filesystem::path& artifact, const platform_signing_options& options)
    -> std::filesystem::path {
    if (!options.output.empty()) {
        return options.output;
    }
    return artifact.parent_path() /
           (artifact.stem().string() + "-signed" + artifact.extension().string());
}

auto output_is_input(const std::filesystem::path& artifact, const std::filesystem::path& output)
    -> result<bool> {
    auto input_code = std::error_code{};
    auto output_code = std::error_code{};
    const auto input_path = std::filesystem::weakly_canonical(artifact, input_code);
    const auto output_path = std::filesystem::weakly_canonical(output, output_code);
    if (input_code) {
        return std::unexpected(failure(errc::io, input_code.message(), artifact.string()));
    }
    if (output_code) {
        return std::unexpected(failure(errc::io, output_code.message(), output.string()));
    }
    return input_path == output_path;
}

auto copy_artifact(const std::filesystem::path& source, const std::filesystem::path& destination)
    -> result<void> {
    auto code = std::error_code{};
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), destination.string()));
    }
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), destination.string()));
    }
    return {};
}

auto identity_value(std::string_view uri, std::string_view scheme) -> result<std::string> {
    const auto prefix = std::string{scheme} + ":";
    if (!uri.starts_with(prefix) || uri.size() == prefix.size()) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "signing identity uses the wrong custody URI",
                                       std::string{uri}));
    }
    return std::string{uri.substr(prefix.size())};
}

auto first_hex_fingerprint(std::string_view value) -> std::string {
    auto candidate = std::string{};
    for (const auto character : value) {
        if (std::isxdigit(static_cast<unsigned char>(character))) {
            candidate.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
            continue;
        }
        if (candidate.size() == 40 || candidate.size() == 64) {
            return candidate;
        }
        candidate.clear();
    }
    return candidate.size() == 40 || candidate.size() == 64 ? candidate : std::string{};
}

auto labelled_sha256_fingerprint(std::string_view value) -> std::string {
    const auto marker = value.find("SHA256 Fingerprint:");
    if (marker == std::string_view::npos) {
        return {};
    }
    auto output = std::string{};
    for (const auto character :
         value.substr(marker + std::string_view{"SHA256 Fingerprint:"}.size())) {
        if (std::isxdigit(static_cast<unsigned char>(character))) {
            output.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
            if (output.size() == 64) {
                return output;
            }
        } else if (!std::isspace(static_cast<unsigned char>(character)) && character != ':') {
            return {};
        }
    }
    return {};
}

auto macos_artifact_fingerprint(const std::filesystem::path& artifact,
                                std::string_view verification) -> result<std::string> {
    if (artifact.extension() == ".pkg") {
        auto fingerprint = labelled_sha256_fingerprint(verification);
        if (fingerprint.empty()) {
            return std::unexpected(failure(errc::corrupt,
                                           "cannot read package signing fingerprint",
                                           std::string{verification}));
        }
        return fingerprint;
    }
    const auto prefix = temporary_path("codesign-certificate");
    auto extracted = run_tool(
        {"codesign", "--display", "--extract-certificates", prefix.string(), artifact.string()});
    if (!extracted) {
        return std::unexpected(extracted.error());
    }
    auto extracted_ok = require_tool(*extracted, "codesign certificate extraction");
    if (!extracted_ok) {
        return std::unexpected(extracted_ok.error());
    }
    const auto certificate = std::filesystem::path{prefix.string() + "0"};
    auto selected = sha256_file(certificate);
    auto code = std::error_code{};
    std::filesystem::remove(certificate, code);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    auto fingerprint = to_string(*selected).substr(std::string_view{"sha256:"}.size());
    std::ranges::transform(fingerprint, fingerprint.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return fingerprint;
}

auto is_macho(const std::filesystem::path& path) -> bool {
    auto input = std::ifstream{path, std::ios::binary};
    auto bytes = std::array<unsigned char, 4>{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return false;
    }
    const auto magic = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                       (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                       (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                       static_cast<std::uint32_t>(bytes[3]);
    return magic == 0xfeedfaceU || magic == 0xfeedfacfU || magic == 0xcefaedfeU ||
           magic == 0xcffaedfeU || magic == 0xcafebabeU || magic == 0xbebafecaU;
}

auto codesign_payload(const std::filesystem::path& expanded,
                      std::string_view identity,
                      const std::filesystem::path& entitlements,
                      bool timestamp) -> result<void> {
    auto code = std::error_code{};
    auto paths = std::vector<std::filesystem::path>{};
    for (auto iterator = std::filesystem::recursive_directory_iterator{expanded, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         ++iterator) {
        if (iterator->is_regular_file(code) && !code && is_macho(iterator->path())) {
            paths.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), expanded.string()));
    }
    std::ranges::sort(paths);
    for (const auto& path : paths) {
        auto command = std::vector<std::string>{"codesign",
                                                "--force",
                                                "--sign",
                                                std::string{identity},
                                                "--options",
                                                "runtime",
                                                timestamp ? "--timestamp" : "--timestamp=none"};
        if (!entitlements.empty()) {
            command.push_back("--entitlements");
            command.push_back(entitlements.string());
        }
        command.push_back(path.string());
        auto signed_value = run_tool(command);
        if (!signed_value) {
            return std::unexpected(signed_value.error());
        }
        auto verified = require_tool(*signed_value, "codesign");
        if (!verified) {
            return std::unexpected(verified.error());
        }
    }
    return {};
}

auto notary_environment_present() -> bool {
    for (const auto* name : {"LIQUID_APPLE_NOTARY_KEY",
                             "LIQUID_APPLE_NOTARY_KEY_ID",
                             "LIQUID_APPLE_NOTARY_ISSUER_ID"}) {
        const auto* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return false;
        }
    }
    return true;
}

auto json_identifier(std::string_view value) -> std::string {
    const auto marker = value.find("\"id\"");
    if (marker == std::string_view::npos) {
        return {};
    }
    const auto colon = value.find(':', marker + 4);
    const auto quote = colon == std::string_view::npos ? colon : value.find('"', colon + 1);
    const auto end = quote == std::string_view::npos ? quote : value.find('"', quote + 1);
    if (quote == std::string_view::npos || end == std::string_view::npos) {
        return {};
    }
    return std::string{value.substr(quote + 1, end - quote - 1)};
}

auto notarize(const std::filesystem::path& artifact)
    -> result<std::pair<std::string, std::string>> {
    const auto key_path = temporary_path("notary-key.p8");
    const auto* key = std::getenv("LIQUID_APPLE_NOTARY_KEY");
    auto written = write_text_file(key_path, key);
    if (!written) {
        auto ignored = std::error_code{};
        std::filesystem::remove(key_path, ignored);
        return std::unexpected(written.error());
    }
    auto code = std::error_code{};
    std::filesystem::permissions(key_path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code) {
        auto ignored = std::error_code{};
        std::filesystem::remove(key_path, ignored);
        return std::unexpected(failure(errc::io, code.message(), key_path.string()));
    }
    auto submitted = run_tool({"xcrun",
                               "notarytool",
                               "submit",
                               artifact.string(),
                               "--wait",
                               "--output-format",
                               "json",
                               "--key",
                               key_path.string(),
                               "--key-id",
                               std::getenv("LIQUID_APPLE_NOTARY_KEY_ID"),
                               "--issuer",
                               std::getenv("LIQUID_APPLE_NOTARY_ISSUER_ID")});
    code.clear();
    std::filesystem::remove(key_path, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), key_path.string()));
    }
    if (!submitted) {
        return std::unexpected(submitted.error());
    }
    auto response = require_tool(*submitted, "notarytool");
    if (!response) {
        return std::unexpected(response.error());
    }
    const auto ticket = json_identifier(*response);
    if (ticket.empty()) {
        return std::unexpected(
            failure(errc::corrupt, "notarytool response has no ticket id", *response));
    }
    auto stapled = run_tool({"xcrun", "stapler", "staple", artifact.string()});
    if (!stapled) {
        return std::unexpected(stapled.error());
    }
    auto staple_response = require_tool(*stapled, "stapler");
    if (!staple_response) {
        return std::unexpected(staple_response.error());
    }
    auto assessed =
        run_tool({"spctl", "--assess", "--type", "install", "--verbose", artifact.string()});
    if (!assessed) {
        return std::unexpected(assessed.error());
    }
    auto assessment = require_tool(*assessed, "spctl");
    if (!assessment) {
        return std::unexpected(assessment.error());
    }
    return std::pair{ticket, *assessment};
}

struct macos_identities {
    std::string installer;
    std::string application;
};

auto parse_macos_identities(std::string_view uri) -> result<macos_identities> {
    auto identity = identity_value(uri, "keychain");
    if (!identity) {
        return std::unexpected(identity.error());
    }
    const auto separator = identity->find('|');
    auto output = macos_identities{.installer = identity->substr(0, separator)};
    output.application =
        separator == std::string::npos ? output.installer : identity->substr(separator + 1);
    if (output.installer.empty() || output.application.empty()) {
        return std::unexpected(
            failure(errc::invalid_argument,
                    "macOS keychain URI must name an installer and optional application identity"));
    }
    return output;
}

auto sign_macos_package(const platform_signing_request& request,
                        const std::filesystem::path& output,
                        const macos_identities& identities,
                        bool timestamp) -> result<std::string> {
    auto code = std::error_code{};
    std::filesystem::remove(output, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), output.string()));
    }
    const auto temporary = temporary_path("pkg-signing");
    const auto expanded = temporary / "expanded";
    const auto rebuilt = temporary / "payload-signed.pkg";
    std::filesystem::create_directories(temporary, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), temporary.string()));
    }
    auto expanded_result =
        run_tool({"pkgutil", "--expand-full", request.artifact.string(), expanded.string()});
    if (!expanded_result) {
        return std::unexpected(expanded_result.error());
    }
    auto expanded_ok = require_tool(*expanded_result, "pkgutil --expand-full");
    if (!expanded_ok) {
        return std::unexpected(expanded_ok.error());
    }
    auto payload =
        codesign_payload(expanded, identities.application, request.entitlements, timestamp);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto flattened = run_tool({"pkgutil", "--flatten-full", expanded.string(), rebuilt.string()});
    if (!flattened) {
        return std::unexpected(flattened.error());
    }
    auto flattened_ok = require_tool(*flattened, "pkgutil --flatten-full");
    if (!flattened_ok) {
        return std::unexpected(flattened_ok.error());
    }
    auto signed_value = run_tool({"productsign",
                                  "--sign",
                                  identities.installer,
                                  timestamp ? "--timestamp" : "--timestamp=none",
                                  rebuilt.string(),
                                  output.string()});
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    auto signed_ok = require_tool(*signed_value, "productsign");
    if (!signed_ok) {
        return std::unexpected(signed_ok.error());
    }
    auto checked = run_tool({"pkgutil", "--check-signature", output.string()});
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto verification = require_tool(*checked, "pkgutil --check-signature");
    if (!verification) {
        return std::unexpected(verification.error());
    }
    auto removed = remove_known_path(temporary);
    if (!removed) {
        return std::unexpected(removed.error());
    }
    return verification;
}

auto sign_macos_dmg(const platform_signing_request& request,
                    const std::filesystem::path& output,
                    const macos_identities& identities,
                    bool timestamp) -> result<std::string> {
    auto copied = copy_artifact(request.artifact, output);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto command = std::vector<std::string>{"codesign",
                                            "--force",
                                            "--sign",
                                            identities.application,
                                            "--options",
                                            "runtime",
                                            timestamp ? "--timestamp" : "--timestamp=none"};
    if (!request.entitlements.empty()) {
        command.push_back("--entitlements");
        command.push_back(request.entitlements.string());
    }
    command.push_back(output.string());
    auto signed_value = run_tool(command);
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    auto signed_ok = require_tool(*signed_value, "codesign");
    if (!signed_ok) {
        return std::unexpected(signed_ok.error());
    }
    auto checked = run_tool({"codesign", "--verify", "--verbose=4", output.string()});
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return require_tool(*checked, "codesign --verify");
}

auto sign_macos(const platform_signing_request& request,
                const platform_signing_options& options,
                const std::filesystem::path& output) -> result<platform_signature_report> {
#if !defined(__APPLE__)
    static_cast<void>(request);
    static_cast<void>(options);
    static_cast<void>(output);
    return std::unexpected(failure(errc::unsupported, "macOS signing requires Apple tools"));
#else
    auto identities = parse_macos_identities(options.identity);
    if (!identities) {
        return std::unexpected(identities.error());
    }
    const auto submit = options.notarize && notary_environment_present();
    auto code = std::error_code{};
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), output.string()));
    }
    auto signed_result = result<std::string>{};
    if (request.artifact.extension() == ".pkg") {
        signed_result = sign_macos_package(request, output, *identities, submit);
    } else if (request.artifact.extension() == ".dmg") {
        signed_result = sign_macos_dmg(request, output, *identities, submit);
    } else {
        return std::unexpected(failure(errc::invalid_argument, "macOS signing needs a pkg or dmg"));
    }
    if (!signed_result) {
        return std::unexpected(signed_result.error());
    }
    auto verification = std::move(*signed_result);
    auto fingerprint = macos_artifact_fingerprint(output, verification);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto ticket = std::string{};
    auto skipped = options.notarize && !notary_environment_present();
    if (submit) {
        auto notarized = notarize(output);
        if (!notarized) {
            return std::unexpected(notarized.error());
        }
        ticket = std::move(notarized->first);
        verification = std::move(notarized->second);
    }
    auto digest_value = sha256_file(output);
    if (!digest_value) {
        return std::unexpected(digest_value.error());
    }
    return platform_signature_report{.artifact = {output,
                                                  *digest_value,
                                                  request.artifact.extension() == ".pkg"
                                                      ? "application/vnd.apple.installer+xml"
                                                      : "application/x-apple-diskimage",
                                                  false},
                                     .unsigned_digest = request.unsigned_digest,
                                     .signing_identity_fingerprint = std::move(*fingerprint),
                                     .platform_verification = std::move(verification),
                                     .notarization_ticket = std::move(ticket),
                                     .notarization_skipped = skipped};
#endif
}

auto windows_fingerprint(const std::filesystem::path& artifact) -> result<std::string> {
    const auto script =
        "(Get-AuthenticodeSignature -LiteralPath $args[0]).SignerCertificate.Thumbprint";
    auto selected = run_tool(
        {"powershell", "-NoProfile", "-NonInteractive", "-Command", script, artifact.string()});
    if (!selected) {
        return std::unexpected(selected.error());
    }
    auto response = require_tool(*selected, "Get-AuthenticodeSignature");
    if (!response) {
        return std::unexpected(response.error());
    }
    auto fingerprint = first_hex_fingerprint(*response);
    if (fingerprint.empty()) {
        return std::unexpected(
            failure(errc::corrupt, "cannot read Authenticode certificate fingerprint", *response));
    }
    return fingerprint;
}

auto sign_windows(const platform_signing_request& request,
                  const platform_signing_options& options,
                  const std::filesystem::path& output) -> result<platform_signature_report> {
#if !defined(_WIN32)
    static_cast<void>(request);
    static_cast<void>(options);
    static_cast<void>(output);
    return std::unexpected(failure(errc::unsupported, "Windows signing requires signtool"));
#else
    auto copied = copy_artifact(request.artifact, output);
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto command = std::vector<std::string>{"signtool", "sign", "/fd", "sha256"};
    if (options.identity.starts_with("file:")) {
        auto pfx = identity_value(options.identity, "file");
        if (!pfx) {
            return std::unexpected(pfx.error());
        }
        command.push_back("/f");
        command.push_back(std::move(*pfx));
        const auto* password = std::getenv("LIQUID_WINDOWS_PFX_PASSWORD");
        if (password != nullptr && *password != '\0') {
            command.push_back("/p");
            command.emplace_back(password);
        }
    } else if (options.identity.starts_with("csp:")) {
        auto selected = identity_value(options.identity, "csp");
        if (!selected) {
            return std::unexpected(selected.error());
        }
        const auto first = selected->find('|');
        const auto second = first == std::string::npos ? first : selected->find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            return std::unexpected(failure(errc::invalid_argument,
                                           "CSP URI must be csp:provider|container|certificate"));
        }
        command.insert(command.end(),
                       {"/csp",
                        selected->substr(0, first),
                        "/kc",
                        selected->substr(first + 1, second - first - 1),
                        "/f",
                        selected->substr(second + 1)});
    } else {
        return std::unexpected(
            failure(errc::invalid_argument, "Windows signing identity must use file: or csp:"));
    }
    if (!options.timestamp_server.empty()) {
        command.insert(command.end(), {"/tr", options.timestamp_server, "/td", "sha256"});
    }
    command.push_back(output.string());
    auto signed_value = run_tool(command);
    std::ranges::fill(command, std::string{});
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    auto signed_ok = require_tool(*signed_value, "signtool sign");
    if (!signed_ok) {
        return std::unexpected(signed_ok.error());
    }
    const auto timestamp_response = options.timestamp_server.empty() ? std::string{} : *signed_ok;
    auto verified = run_tool({"signtool", "verify", "/pa", "/v", output.string()});
    if (!verified) {
        return std::unexpected(verified.error());
    }
    auto verification = require_tool(*verified, "signtool verify");
    if (!verification) {
        return std::unexpected(verification.error());
    }
    auto fingerprint = windows_fingerprint(output);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto digest_value = sha256_file(output);
    if (!digest_value) {
        return std::unexpected(digest_value.error());
    }
    return platform_signature_report{.artifact = {output, *digest_value, "application/msix", false},
                                     .unsigned_digest = request.unsigned_digest,
                                     .signing_identity_fingerprint = std::move(*fingerprint),
                                     .platform_verification = std::move(*verification),
                                     .timestamp_response = timestamp_response};
#endif
}

struct native_signature_verification {
    platform_signing_system platform{platform_signing_system::macos};
    std::string output;
    std::string fingerprint;
};

auto verify_native_signature(const std::filesystem::path& artifact,
                             std::string_view platform,
                             bool require_gatekeeper) -> result<native_signature_verification> {
    if (platform == "macos") {
#if defined(__APPLE__)
        auto command =
            artifact.extension() == ".pkg"
                ? std::vector<std::string>{"pkgutil", "--check-signature", artifact.string()}
                : std::vector<std::string>{
                      "codesign", "--verify", "--verbose=4", artifact.string()};
        auto checked = run_tool(command);
        if (!checked) {
            return std::unexpected(checked.error());
        }
        auto verification = require_tool(*checked, "platform verifier");
        if (!verification) {
            return std::unexpected(verification.error());
        }
        auto fingerprint = macos_artifact_fingerprint(artifact, *verification);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        if (require_gatekeeper) {
            auto assessed = run_tool(
                {"spctl", "--assess", "--type", "install", "--verbose", artifact.string()});
            if (!assessed) {
                return std::unexpected(assessed.error());
            }
            auto assessment = require_tool(*assessed, "spctl");
            if (!assessment) {
                return std::unexpected(assessment.error());
            }
            verification->append("\n" + *assessment);
        }
        return native_signature_verification{
            platform_signing_system::macos, std::move(*verification), std::move(*fingerprint)};
#else
        static_cast<void>(artifact);
        static_cast<void>(require_gatekeeper);
        return std::unexpected(failure(errc::unsupported, "macOS verifier is unavailable"));
#endif
    }
    if (platform == "windows") {
#if defined(_WIN32)
        auto checked = run_tool({"signtool", "verify", "/pa", "/v", artifact.string()});
        if (!checked) {
            return std::unexpected(checked.error());
        }
        auto verification = require_tool(*checked, "platform verifier");
        if (!verification) {
            return std::unexpected(verification.error());
        }
        auto fingerprint = windows_fingerprint(artifact);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        return native_signature_verification{
            platform_signing_system::windows, std::move(*verification), std::move(*fingerprint)};
#else
        static_cast<void>(artifact);
        return std::unexpected(failure(errc::unsupported, "Windows verifier is unavailable"));
#endif
    }
    return std::unexpected(
        failure(errc::corrupt, "unknown platform signature system", std::string{platform}));
}

auto attestation_attribute_text(const attestation& value, std::string_view name)
    -> result<std::string> {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    if (found == value.attributes.end()) {
        return std::unexpected(
            failure(errc::corrupt, "platform signature attribute is missing", std::string{name}));
    }
    const auto* selected = std::get_if<std::string>(&found->value);
    if (!selected) {
        return std::unexpected(
            failure(errc::corrupt, "platform signature attribute is not text", std::string{name}));
    }
    return *selected;
}

} // namespace
} // namespace liquid::projection_detail

namespace liquid {

runtime::task<platform_signature_report> sign_platform(repository& source,
                                                       std::filesystem::path artifact,
                                                       platform_signing_options options) {
    auto request =
        co_await runtime::offload([artifact] { return projection_detail::read_request(artifact); });
    if (!request) {
        co_return std::unexpected(request.error());
    }
    if (!*request) {
        co_return std::unexpected(request->error());
    }
    auto current =
        co_await runtime::offload([artifact] { return projection_detail::sha256_file(artifact); });
    if (!current) {
        co_return std::unexpected(current.error());
    }
    if (!*current) {
        co_return std::unexpected(current->error());
    }
    if (to_string(**current) != to_string((*request)->unsigned_digest)) {
        co_return std::unexpected(error{errc::conflict,
                                        "unsigned artifact differs from its signing request",
                                        artifact.string()});
    }
    const auto output = projection_detail::signed_path(artifact, options);
    auto signed_value = co_await runtime::offload([request = **request, options, output] {
        auto same_path = projection_detail::output_is_input(request.artifact, output);
        if (!same_path) {
            return result<platform_signature_report>{std::unexpected(same_path.error())};
        }
        if (*same_path) {
            return result<platform_signature_report>{
                std::unexpected(error{errc::invalid_argument,
                                      "signed output must differ from the unsigned artifact",
                                      output.string()})};
        }
        if (request.platform == platform_signing_system::macos) {
            return projection_detail::sign_macos(request, options, output);
        }
        return projection_detail::sign_windows(request, options, output);
    });
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    if (!*signed_value) {
        co_return std::unexpected(signed_value->error());
    }
    auto report = std::move(**signed_value);
    auto statement = attestation{
        .subject = report.artifact.digest,
        .profile = "platform_signature/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"platform", std::string{to_string((*request)->platform)}},
                       {"unsigned_digest", to_string(report.unsigned_digest)},
                       {"signed_digest", to_string(report.artifact.digest)},
                       {"signing_identity", report.signing_identity_fingerprint},
                       {"verification", report.platform_verification}}};
    if (!report.notarization_ticket.empty()) {
        statement.attributes.push_back({"notarization_ticket", report.notarization_ticket});
    }
    if (!report.timestamp_response.empty()) {
        statement.attributes.push_back({"timestamp_response", report.timestamp_response});
    }
    auto signed_statement =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    auto stored = co_await source.store_attestation(statement);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto encoded = encode_attestation(statement);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    report.attestation = projection_detail::attestation_path(report.artifact.path);
    auto written =
        co_await runtime::offload([path = report.attestation, bytes = std::move(*encoded)] {
            return projection_detail::write_file(path, bytes);
        });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    co_return report;
}

runtime::task<platform_signature_verification>
verify_platform_signature(std::filesystem::path artifact) {
    auto encoded = co_await runtime::offload([artifact] {
        return projection_detail::read_file(projection_detail::attestation_path(artifact));
    });
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    if (!*encoded) {
        co_return std::unexpected(encoded->error());
    }
    auto statement = decode_attestation(**encoded);
    if (!statement) {
        co_return std::unexpected(statement.error());
    }
    auto verified = verify_attestation(*statement);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    if (!verified->verified || statement->profile != "platform_signature/1") {
        co_return std::unexpected(error{
            errc::unauthorized, "platform signature attestation is invalid", verified->reason});
    }
    auto signed_digest =
        co_await runtime::offload([artifact] { return projection_detail::sha256_file(artifact); });
    if (!signed_digest) {
        co_return std::unexpected(signed_digest.error());
    }
    if (!*signed_digest) {
        co_return std::unexpected(signed_digest->error());
    }
    auto recorded_digest =
        projection_detail::attestation_attribute_text(*statement, "signed_digest");
    auto platform = projection_detail::attestation_attribute_text(*statement, "platform");
    auto fingerprint =
        projection_detail::attestation_attribute_text(*statement, "signing_identity");
    if (!recorded_digest || !platform || !fingerprint) {
        co_return std::unexpected(!recorded_digest ? recorded_digest.error()
                                  : !platform      ? platform.error()
                                                   : fingerprint.error());
    }
    if (*recorded_digest != to_string(**signed_digest) ||
        to_string(statement->subject) != to_string(**signed_digest)) {
        co_return std::unexpected(error{errc::conflict,
                                        "signed artifact digest differs from its attestation",
                                        artifact.string()});
    }
    const auto optional = [&](std::string_view name) {
        auto value = projection_detail::attestation_attribute_text(*statement, name);
        return value ? std::move(*value) : std::string{};
    };
    auto notarization_ticket = optional("notarization_ticket");
    auto native = co_await runtime::offload([artifact,
                                             platform = *platform,
                                             require_gatekeeper = !notarization_ticket.empty()] {
        return projection_detail::verify_native_signature(artifact, platform, require_gatekeeper);
    });
    if (!native) {
        co_return std::unexpected(native.error());
    }
    if (!*native) {
        co_return std::unexpected(native->error());
    }
    if ((*native)->fingerprint != *fingerprint) {
        co_return std::unexpected(error{
            errc::conflict, "signing certificate differs from its attestation", artifact.string()});
    }
    co_return platform_signature_verification{.platform = (*native)->platform,
                                              .signed_digest = **signed_digest,
                                              .signing_identity_fingerprint =
                                                  std::move(*fingerprint),
                                              .platform_verification = std::move((*native)->output),
                                              .notarization_ticket = std::move(notarization_ticket),
                                              .timestamp_response = optional("timestamp_response")};
}

} // namespace liquid
