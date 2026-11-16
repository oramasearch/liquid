// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>

#include <zlib.h>

#include "git/sha1.hpp"
#include "projection/platform_tools.hpp"

namespace liquid::projection_detail {
namespace {

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto xml(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
        switch (character) {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        case '\'':
            output += "&apos;";
            break;
        default:
            output.push_back(character);
        }
    }
    return output;
}

auto yaml(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

auto json(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
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
            output.push_back(character);
        }
    }
    output.push_back('"');
    return output;
}

auto package_identifier(const package_metadata& metadata) -> std::string {
    if (!metadata.identifier.empty()) {
        return metadata.identifier;
    }
    auto output = std::string{"dev.liquid."};
    for (const auto character : metadata.name) {
        const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        output.push_back(std::isalnum(static_cast<unsigned char>(lowered)) ? lowered : '-');
    }
    return output;
}

auto winget_identifier_is_valid(std::string_view value) -> bool {
    return value.size() <= 128 && value.contains('.') && value.front() != '.' &&
           value.back() != '.' && !value.contains("..") &&
           std::ranges::all_of(value, [](unsigned char character) {
               return std::isalnum(character) || character == '.' || character == '-';
           });
}

auto yaml_scalar_is_safe(std::string_view value) -> bool {
    return value.find_first_of("\r\n") == std::string_view::npos &&
           value.find('\0') == std::string_view::npos;
}

auto windows_name(const package_metadata& metadata) -> std::string {
    auto output = package_identifier(metadata);
    output.erase(std::remove_if(output.begin(),
                                output.end(),
                                [](unsigned char character) {
                                    return !std::isalnum(character) && character != '.' &&
                                           character != '-';
                                }),
                 output.end());
    if (output.size() < 3) {
        output = "liquid." + metadata.name;
    }
    if (output.size() > 50) {
        output.resize(50);
    }
    return output;
}

auto windows_version(std::string_view value) -> result<std::string> {
    auto parts = std::vector<std::string>{};
    auto offset = std::size_t{};
    while (offset <= value.size()) {
        const auto end = value.find('.', offset);
        const auto part = value.substr(offset, end - offset);
        if (part.empty() || !std::ranges::all_of(part, [](unsigned char character) {
                return std::isdigit(character);
            })) {
            return std::unexpected(failure(errc::invalid_argument,
                                           "MSIX version must contain decimal components",
                                           std::string{value}));
        }
        parts.emplace_back(part);
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    if (parts.size() > 4) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "MSIX version has more than four components",
                                       std::string{value}));
    }
    while (parts.size() < 4) {
        parts.emplace_back("0");
    }
    return parts[0] + "." + parts[1] + "." + parts[2] + "." + parts[3];
}

auto windows_architecture(std::string_view value) -> result<std::string> {
    if (value == "x86_64" || value == "amd64" || value == "x64") {
        return "x64";
    }
    if (value == "aarch64" || value == "arm64") {
        return "arm64";
    }
    if (value == "x86" || value == "neutral") {
        return std::string{value};
    }
    return std::unexpected(
        failure(errc::invalid_argument, "unsupported MSIX architecture", std::string{value}));
}

auto display_name(const package_metadata& metadata) -> std::string_view {
    return metadata.display_name.empty() ? std::string_view{metadata.name}
                                         : std::string_view{metadata.display_name};
}

auto executable_name(const package_metadata& metadata) -> std::string {
    auto output =
        metadata.executable.empty() ? "bin/" + metadata.name + ".exe" : metadata.executable;
    std::ranges::replace(output, '/', '\\');
    return output;
}

auto publisher(const package_metadata& metadata) -> std::string_view {
    return metadata.publisher.empty() ? std::string_view{"CN=Liquid Development"}
                                      : std::string_view{metadata.publisher};
}

auto set_date(const std::filesystem::path& root, std::uint64_t epoch) -> result<void> {
    auto code = std::error_code{};
    const auto time = std::chrono::sys_seconds{std::chrono::seconds{epoch}};
    const auto file_time = std::filesystem::file_time_type::clock::from_sys(time);
    std::filesystem::last_write_time(root, file_time, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), root.string()));
    }
    if (!std::filesystem::is_directory(root, code)) {
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), root.string()));
        }
        return {};
    }
    for (auto iterator = std::filesystem::recursive_directory_iterator{root, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         ++iterator) {
        std::filesystem::last_write_time(iterator->path(), file_time, code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), root.string()));
    }
    return {};
}

auto read_be64(std::span<const std::byte> value, std::size_t offset) -> std::uint64_t {
    auto output = std::uint64_t{};
    for (auto index = std::size_t{}; index < sizeof(output); ++index) {
        output = (output << 8U) | std::to_integer<std::uint8_t>(value[offset + index]);
    }
    return output;
}

void write_be64(std::span<std::byte> value, std::size_t offset, std::uint64_t selected) {
    for (auto index = std::size_t{}; index < sizeof(selected); ++index) {
        const auto shift = (sizeof(selected) - index - 1U) * 8U;
        value[offset + index] = std::byte{static_cast<std::uint8_t>(selected >> shift)};
    }
}

auto iso8601(std::uint64_t epoch) -> std::string {
    const auto moment = std::chrono::sys_seconds{std::chrono::seconds{epoch}};
    const auto day = std::chrono::floor<std::chrono::days>(moment);
    const auto date = std::chrono::year_month_day{day};
    const auto time = std::chrono::hh_mm_ss{moment - day};
    auto output = std::ostringstream{};
    output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
           << std::setw(2) << static_cast<unsigned>(date.month()) << '-' << std::setw(2)
           << static_cast<unsigned>(date.day()) << 'T' << std::setw(2) << time.hours().count()
           << ':' << std::setw(2) << time.minutes().count() << ':' << std::setw(2)
           << time.seconds().count();
    return output.str();
}

auto inflate_xar_toc(std::span<const std::byte> compressed, std::size_t size)
    -> result<std::vector<std::byte>> {
    if (compressed.size() > std::numeric_limits<uInt>::max() ||
        size > std::numeric_limits<uInt>::max()) {
        return std::unexpected(failure(errc::unsupported, "xar table of contents is too large"));
    }
    auto output = std::vector<std::byte>(size);
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit(&stream) != Z_OK) {
        return std::unexpected(failure(errc::internal, "cannot initialize xar decoder"));
    }
    const auto status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != output.size()) {
        return std::unexpected(failure(errc::corrupt, "invalid xar table of contents"));
    }
    return output;
}

auto deflate_xar_toc(std::span<const std::byte> value) -> result<std::vector<std::byte>> {
    if (value.size() > std::numeric_limits<uLong>::max()) {
        return std::unexpected(failure(errc::unsupported, "xar table of contents is too large"));
    }
    auto size = compressBound(static_cast<uLong>(value.size()));
    auto output = std::vector<std::byte>(size);
    const auto status = compress2(reinterpret_cast<Bytef*>(output.data()),
                                  &size,
                                  reinterpret_cast<const Bytef*>(value.data()),
                                  static_cast<uLong>(value.size()),
                                  Z_BEST_COMPRESSION);
    if (status != Z_OK) {
        return std::unexpected(failure(errc::internal, "cannot encode xar table of contents"));
    }
    output.resize(size);
    return output;
}

auto replace_element_contents(std::string& document, std::string_view name, std::string_view value)
    -> bool {
    const auto opening_tag = "<" + std::string{name} + ">";
    const auto closing_tag = "</" + std::string{name} + ">";
    auto offset = std::size_t{};
    auto replaced = false;
    while ((offset = document.find(opening_tag, offset)) != std::string::npos) {
        const auto content = offset + opening_tag.size();
        const auto closing = document.find(closing_tag, content);
        if (closing == std::string::npos) {
            return false;
        }
        document.replace(content, closing - content, value);
        offset = content + value.size() + closing_tag.size();
        replaced = true;
    }
    return replaced;
}

auto normalise_xar_metadata(const std::filesystem::path& path, std::uint64_t epoch)
    -> result<void> {
    constexpr auto header_size = std::size_t{28};
    constexpr auto sha1_size = std::size_t{20};
    auto archive = read_file(path);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    if (archive->size() < header_size + sha1_size ||
        std::string_view{reinterpret_cast<const char*>(archive->data()), 4} != "xar!") {
        return std::unexpected(failure(errc::corrupt, "invalid xar header", path.string()));
    }
    const auto compressed_size = read_be64(*archive, 8);
    const auto uncompressed_size = read_be64(*archive, 16);
    if (compressed_size > archive->size() - header_size - sha1_size) {
        return std::unexpected(failure(errc::corrupt, "invalid xar table size", path.string()));
    }
    const auto compressed =
        std::span<const std::byte>{*archive}.subspan(header_size, compressed_size);
    auto toc = inflate_xar_toc(compressed, uncompressed_size);
    if (!toc) {
        return std::unexpected(toc.error());
    }
    auto xml_value = std::string{reinterpret_cast<const char*>(toc->data()), toc->size()};
    const auto timestamp = iso8601(epoch);
    if (!replace_element_contents(xml_value, "creation-time", timestamp)) {
        return std::unexpected(
            failure(errc::corrupt, "xar creation time is missing", path.string()));
    }
    for (const auto field : {"ctime", "mtime", "atime", "FinderCreateTime"}) {
        replace_element_contents(xml_value, field, timestamp + "Z");
    }
    for (const auto field : {"inode", "deviceno", "uid", "gid"}) {
        replace_element_contents(xml_value, field, "0");
    }
    replace_element_contents(xml_value, "user", "root");
    replace_element_contents(xml_value, "group", "wheel");
    toc->assign(reinterpret_cast<const std::byte*>(xml_value.data()),
                reinterpret_cast<const std::byte*>(xml_value.data() + xml_value.size()));
    auto encoded = deflate_xar_toc(*toc);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto output = std::vector<std::byte>{archive->begin(), archive->begin() + header_size};
    write_be64(output, 8, encoded->size());
    write_be64(output, 16, toc->size());
    output.insert(output.end(), encoded->begin(), encoded->end());
    const auto checksum = git_detail::sha1_hash(*encoded);
    output.insert(output.end(), checksum.begin(), checksum.end());
    const auto heap = header_size + compressed_size + sha1_size;
    output.insert(
        output.end(), archive->begin() + static_cast<std::ptrdiff_t>(heap), archive->end());
    return write_file(path, output);
}

auto materialise(const prepared_projection& prepared, const std::filesystem::path& root)
    -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(root, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), root.string()));
    }
    for (const auto& file : prepared.files) {
        const auto path = root / file.path;
        std::filesystem::create_directories(path.parent_path(), code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.parent_path().string()));
        }
        auto written = write_file(path, file.bytes);
        if (!written) {
            return std::unexpected(written.error());
        }
        std::filesystem::permissions(path,
                                     static_cast<std::filesystem::perms>(file.mode),
                                     std::filesystem::perm_options::replace,
                                     code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.string()));
        }
    }
    return set_date(root, prepared.options.source_date_epoch);
}

auto require_tool(const tool_result& execution, std::string_view tool) -> result<void> {
    if (execution.exit == 0) {
        return {};
    }
    auto diagnostic = execution.error_output.empty() ? execution.output : execution.error_output;
    while (!diagnostic.empty() && (diagnostic.back() == '\n' || diagnostic.back() == '\r')) {
        diagnostic.pop_back();
    }
    return std::unexpected(failure(errc::io, std::string{tool} + " failed", std::move(diagnostic)));
}

auto signing_request_path(const std::filesystem::path& artifact) -> std::filesystem::path {
    return artifact.string() + ".liquid-signing-request.json";
}

auto write_signing_request(platform_signing_request request) -> result<platform_signing_request> {
    const auto encoded = "{\"schema\":\"platform_signing_request/1\",\"platform\":" +
                         json(to_string(request.platform)) +
                         ",\"artifact\":" + json(request.artifact.generic_string()) +
                         ",\"unsigned_digest\":" + json(to_string(request.unsigned_digest)) +
                         ",\"identifier\":" + json(request.identifier) +
                         ",\"executable\":" + json(request.executable) +
                         ",\"entitlements\":" + json(request.entitlements.generic_string()) + "}\n";
    auto written = write_text_file(request.path, encoded);
    if (!written) {
        return std::unexpected(written.error());
    }
    return request;
}

auto projection_attestation(repository& source,
                            const prepared_projection& prepared,
                            projection_target target,
                            digest subject) -> runtime::task<std::vector<digest>> {
    auto id = co_await store_projection_attestation(source, prepared, target, subject);
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto output = prepared.source_attestations;
    output.push_back(*id);
    co_return output;
}

auto hex(digest value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789ABCDEF"};
    auto output = std::string{};
    output.reserve(value.bytes.size() * 2);
    for (const auto byte : value.bytes) {
        const auto selected = std::to_integer<unsigned char>(byte);
        output.push_back(digits[selected >> 4U]);
        output.push_back(digits[selected & 0x0fU]);
    }
    return output;
}

auto write_mapping(const std::filesystem::path& root,
                   const std::filesystem::path& manifest,
                   const std::filesystem::path& path) -> result<void> {
    auto mapping = std::string{"[Files]\r\n"};
    mapping += "\"" + manifest.string() + "\" \"AppxManifest.xml\"\r\n";
    auto files = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator = std::filesystem::recursive_directory_iterator{root, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         ++iterator) {
        if (iterator->is_regular_file()) {
            files.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), root.string()));
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
        auto destination = std::filesystem::relative(file, root, code).generic_string();
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), file.string()));
        }
        std::ranges::replace(destination, '/', '\\');
        mapping += "\"" + file.string() + "\" \"" + destination + "\"\r\n";
    }
    return write_text_file(path, mapping);
}

void append_be32(std::vector<std::byte>& output, std::uint32_t value) {
    for (auto index = 0U; index < 4U; ++index) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> ((3U - index) * 8U))});
    }
}

void append_png_chunk(std::vector<std::byte>& output,
                      std::string_view type,
                      std::span<const std::byte> value) {
    append_be32(output, static_cast<std::uint32_t>(value.size()));
    const auto type_offset = output.size();
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(type.data()),
                  reinterpret_cast<const std::byte*>(type.data() + type.size()));
    output.insert(output.end(), value.begin(), value.end());
    auto checksum = crc32(0, nullptr, 0);
    checksum = crc32(checksum,
                     reinterpret_cast<const Bytef*>(output.data() + type_offset),
                     static_cast<uInt>(type.size() + value.size()));
    append_be32(output, checksum);
}

auto png(std::uint32_t width, std::uint32_t height) -> result<std::vector<std::byte>> {
    auto rows = std::vector<std::byte>(static_cast<std::size_t>(height) * (width * 4U + 1U));
    auto compressed_size = compressBound(rows.size());
    auto compressed = std::vector<std::byte>(compressed_size);
    const auto compressed_status = compress2(reinterpret_cast<Bytef*>(compressed.data()),
                                             &compressed_size,
                                             reinterpret_cast<const Bytef*>(rows.data()),
                                             rows.size(),
                                             Z_BEST_COMPRESSION);
    if (compressed_status != Z_OK) {
        return std::unexpected(failure(errc::internal, "cannot encode MSIX logo"));
    }
    compressed.resize(compressed_size);
    auto output = std::vector<std::byte>{std::byte{0x89},
                                         std::byte{0x50},
                                         std::byte{0x4e},
                                         std::byte{0x47},
                                         std::byte{0x0d},
                                         std::byte{0x0a},
                                         std::byte{0x1a},
                                         std::byte{0x0a}};
    auto header = std::vector<std::byte>{};
    append_be32(header, width);
    append_be32(header, height);
    header.insert(header.end(),
                  {std::byte{8}, std::byte{6}, std::byte{0}, std::byte{0}, std::byte{0}});
    append_png_chunk(output, "IHDR", header);
    append_png_chunk(output, "IDAT", compressed);
    append_png_chunk(output, "IEND", {});
    return output;
}

auto write_msix_logos(const std::filesystem::path& root) -> result<void> {
    const auto logos = std::array{std::pair{"StoreLogo.png", 50U},
                                  std::pair{"Square150x150Logo.png", 150U},
                                  std::pair{"Square44x44Logo.png", 44U}};
    for (const auto& [name, size] : logos) {
        auto bytes = png(size, size);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto written = write_file(root / "Assets" / name, *bytes);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return {};
}

auto run_checked(std::vector<std::string> command,
                 std::string_view name,
                 const std::filesystem::path& working_directory = {},
                 const std::map<std::string, std::string>& environment = {}) -> result<void> {
    auto execution = run_tool(command, working_directory, environment);
    if (!execution) {
        return std::unexpected(execution.error());
    }
    return require_tool(*execution, name);
}

auto create_parent(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    return {};
}

auto installer_digest(const std::filesystem::path& path) -> result<digest> {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    auto code = std::error_code{};
    if (extension != ".msix" || !std::filesystem::is_regular_file(path, code)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "winget source must be an MSIX file",
                                       code ? code.message() : path.string()));
    }
    return sha256_file(path);
}

struct platform_projection_output {
    std::filesystem::path artifact;
    std::string media_type;
    bool reproducible{};
    platform_signing_system platform{platform_signing_system::macos};
    std::string identifier;
    std::string executable;
    std::filesystem::path entitlements;
};

auto make_macos_package(const prepared_projection& prepared,
                        const std::filesystem::path& temporary,
                        const std::filesystem::path& root,
                        const std::map<std::string, std::string>& environment)
    -> result<platform_projection_output> {
    const auto component = temporary / "component.pkg";
    const auto product = temporary / "product.pkg";
    const auto expanded = temporary / "expanded";
    auto stripped = run_checked({"xattr", "-cr", root.string()}, "xattr", {}, environment);
    if (!stripped) {
        return std::unexpected(stripped.error());
    }
    auto component_result = run_checked({"pkgbuild",
                                         "--root",
                                         root.string(),
                                         "--identifier",
                                         package_identifier(prepared.metadata),
                                         "--version",
                                         prepared.metadata.version,
                                         "--install-location",
                                         "/",
                                         "--ownership",
                                         "recommended",
                                         "--timestamp=none",
                                         component.string()},
                                        "pkgbuild",
                                        {},
                                        environment);
    if (!component_result) {
        return std::unexpected(component_result.error());
    }
    auto product_result = run_checked(
        {"productbuild", "--package", component.string(), "--timestamp=none", product.string()},
        "productbuild",
        {},
        environment);
    if (!product_result) {
        return std::unexpected(product_result.error());
    }
    auto expanded_result = run_checked({"pkgutil", "--expand", product.string(), expanded.string()},
                                       "pkgutil --expand",
                                       {},
                                       environment);
    if (!expanded_result) {
        return std::unexpected(expanded_result.error());
    }
    auto dated = set_date(expanded, prepared.options.source_date_epoch);
    if (!dated) {
        return std::unexpected(dated.error());
    }
    auto expanded_stripped =
        run_checked({"xattr", "-cr", expanded.string()}, "xattr", {}, environment);
    if (!expanded_stripped) {
        return std::unexpected(expanded_stripped.error());
    }
    const auto artifact = prepared.options.output_directory /
                          (prepared.metadata.name + "-" + prepared.metadata.version + ".pkg");
    auto parent = create_parent(artifact);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    auto packed = run_checked({"pkgutil", "--flatten", expanded.string(), artifact.string()},
                              "pkgutil --flatten",
                              {},
                              environment);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    auto normalised = normalise_xar_metadata(artifact, prepared.options.source_date_epoch);
    if (!normalised) {
        return std::unexpected(normalised.error());
    }
    return platform_projection_output{.artifact = artifact,
                                      .media_type = "application/vnd.apple.installer+xml",
                                      .reproducible = true,
                                      .platform = platform_signing_system::macos,
                                      .identifier = package_identifier(prepared.metadata),
                                      .executable = prepared.metadata.executable,
                                      .entitlements = prepared.options.entitlements};
}

auto make_macos_dmg(const prepared_projection& prepared,
                    const std::filesystem::path& root,
                    const std::map<std::string, std::string>& environment)
    -> result<platform_projection_output> {
    const auto artifact = prepared.options.output_directory /
                          (prepared.metadata.name + "-" + prepared.metadata.version + ".dmg");
    auto parent = create_parent(artifact);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    auto image = run_checked({"hdiutil",
                              "create",
                              "-srcfolder",
                              root.string(),
                              "-volname",
                              std::string{display_name(prepared.metadata)},
                              "-fs",
                              "HFS+",
                              "-format",
                              "UDZO",
                              "-ov",
                              artifact.string()},
                             "hdiutil",
                             {},
                             environment);
    if (!image) {
        return std::unexpected(image.error());
    }
    return platform_projection_output{.artifact = artifact,
                                      .media_type = "application/x-apple-diskimage",
                                      .reproducible = false,
                                      .platform = platform_signing_system::macos,
                                      .identifier = package_identifier(prepared.metadata),
                                      .executable = prepared.metadata.executable,
                                      .entitlements = prepared.options.entitlements};
}

auto make_macos_artifact(const prepared_projection& prepared,
                         projection_target target,
                         const std::filesystem::path& temporary)
    -> result<platform_projection_output> {
    const auto root = temporary / "root";
    auto materialised = materialise(prepared, root);
    if (!materialised) {
        return std::unexpected(materialised.error());
    }
    const auto environment = std::map<std::string, std::string>{
        {"COPYFILE_DISABLE", "1"},
        {"SOURCE_DATE_EPOCH", std::to_string(prepared.options.source_date_epoch)},
        {"TZ", "UTC"},
        {"ZERO_AR_DATE", "1"}};
    if (target == projection_target::macos_pkg) {
        return make_macos_package(prepared, temporary, root, environment);
    }
    return make_macos_dmg(prepared, root, environment);
}

auto make_msix_artifact(const prepared_projection& prepared, const std::filesystem::path& temporary)
    -> result<platform_projection_output> {
    const auto root = temporary / "root";
    auto materialised = materialise(prepared, root);
    if (!materialised) {
        return std::unexpected(materialised.error());
    }
    auto logos = write_msix_logos(root);
    if (!logos) {
        return std::unexpected(logos.error());
    }
    auto dated = set_date(root, prepared.options.source_date_epoch);
    if (!dated) {
        return std::unexpected(dated.error());
    }
    auto manifest = appx_manifest(prepared.metadata);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    const auto manifest_path = temporary / "AppxManifest.xml";
    auto manifest_written = write_text_file(manifest_path, *manifest);
    if (!manifest_written) {
        return std::unexpected(manifest_written.error());
    }
    auto manifest_dated = set_date(manifest_path, prepared.options.source_date_epoch);
    if (!manifest_dated) {
        return std::unexpected(manifest_dated.error());
    }
    const auto mapping = temporary / "mapping.txt";
    auto mapping_written = write_mapping(root, manifest_path, mapping);
    if (!mapping_written) {
        return std::unexpected(mapping_written.error());
    }
    const auto artifact = prepared.options.output_directory /
                          (prepared.metadata.name + "-" + prepared.metadata.version + ".msix");
    auto parent = create_parent(artifact);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    auto packed = run_checked(
        {"makeappx", "pack", "/o", "/f", mapping.string(), "/p", artifact.string()}, "makeappx");
    if (!packed) {
        return std::unexpected(packed.error());
    }
    return platform_projection_output{.artifact = artifact,
                                      .media_type = "application/msix",
                                      .reproducible = true,
                                      .platform = platform_signing_system::windows,
                                      .identifier = package_identifier(prepared.metadata),
                                      .executable = executable_name(prepared.metadata)};
}

auto complete_platform_projection(repository& source,
                                  projection_target target,
                                  const prepared_projection& prepared,
                                  const std::filesystem::path& temporary,
                                  platform_projection_output output)
    -> runtime::task<projection_report> {
    auto artifact_digest =
        co_await runtime::offload([artifact = output.artifact] { return sha256_file(artifact); });
    if (!artifact_digest) {
        co_return std::unexpected(artifact_digest.error());
    }
    if (!*artifact_digest) {
        co_return std::unexpected(artifact_digest->error());
    }
    auto request = platform_signing_request{.path = signing_request_path(output.artifact),
                                            .artifact = output.artifact,
                                            .unsigned_digest = **artifact_digest,
                                            .platform = output.platform,
                                            .identifier = std::move(output.identifier),
                                            .executable = std::move(output.executable),
                                            .entitlements = std::move(output.entitlements)};
    auto written_request = co_await runtime::offload([request = std::move(request)]() mutable {
        return write_signing_request(std::move(request));
    });
    if (!written_request || !*written_request) {
        co_return std::unexpected(written_request ? written_request->error()
                                                  : written_request.error());
    }
    auto attestations =
        co_await projection_attestation(source, prepared, target, **artifact_digest);
    if (!attestations) {
        co_return std::unexpected(attestations.error());
    }
    auto removed = co_await runtime::offload([temporary] { return remove_known_path(temporary); });
    if (!removed || !*removed) {
        co_return std::unexpected(removed ? removed->error() : removed.error());
    }
    co_return projection_report{.artifacts = {{output.artifact,
                                               **artifact_digest,
                                               std::move(output.media_type),
                                               output.reproducible}},
                                .signing_requests = {std::move(**written_request)},
                                .attestations = std::move(*attestations),
                                .pinned = {{prepared.metadata.name, **artifact_digest}},
                                .abi_break_allowed = prepared.abi_break_allowed};
}

} // namespace

result<std::string> appx_manifest(const package_metadata& metadata) {
    auto version = windows_version(metadata.version);
    if (!version) {
        return std::unexpected(version.error());
    }
    auto architecture = windows_architecture(metadata.architecture);
    if (!architecture) {
        return std::unexpected(architecture.error());
    }
    const auto description = metadata.description.empty() ? display_name(metadata)
                                                          : std::string_view{metadata.description};
    auto output = std::string{};
    output += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    output += "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\" ";
    output += "xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\" ";
    output += "xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/"
              "restrictedcapabilities\" ";
    output += "IgnorableNamespaces=\"uap rescap\">\r\n";
    output += "  <Identity Name=\"" + xml(windows_name(metadata)) + "\" Publisher=\"" +
              xml(publisher(metadata)) + "\" Version=\"" + *version +
              "\" ProcessorArchitecture=\"" + *architecture + "\" />\r\n";
    output += "  <Properties>\r\n";
    output += "    <DisplayName>" + xml(display_name(metadata)) + "</DisplayName>\r\n";
    output +=
        "    <PublisherDisplayName>" + xml(display_name(metadata)) + "</PublisherDisplayName>\r\n";
    output += "    <Description>" + xml(description) + "</Description>\r\n";
    output += "    <Logo>Assets\\StoreLogo.png</Logo>\r\n";
    output += "  </Properties>\r\n";
    output += "  <Resources><Resource Language=\"en-us\" /></Resources>\r\n";
    output += "  <Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\" ";
    output += "MinVersion=\"10.0.17763.0\" MaxVersionTested=\"10.0.26100.0\" />";
    output += "</Dependencies>\r\n";
    output += "  <Applications>\r\n";
    output += "    <Application Id=\"App\" Executable=\"" + xml(executable_name(metadata)) +
              "\" EntryPoint=\"Windows.FullTrustApplication\">\r\n";
    output += "      <uap:VisualElements DisplayName=\"" + xml(display_name(metadata)) +
              "\" Description=\"" + xml(description) + "\" BackgroundColor=\"transparent\" ";
    output += "Square150x150Logo=\"Assets\\Square150x150Logo.png\" ";
    output += "Square44x44Logo=\"Assets\\Square44x44Logo.png\" />\r\n";
    output += "    </Application>\r\n";
    output += "  </Applications>\r\n";
    output += "  <Capabilities><rescap:Capability Name=\"runFullTrust\" /></Capabilities>\r\n";
    output += "</Package>\r\n";
    return output;
}

result<std::map<std::string, std::string>> winget_manifests(const package_metadata& metadata,
                                                            std::string_view installer_url,
                                                            digest installer) {
    if (metadata.name.empty() || metadata.version.empty()) {
        return std::unexpected(
            failure(errc::invalid_argument, "winget projection requires a name and version"));
    }
    if (!yaml_scalar_is_safe(metadata.version) || !yaml_scalar_is_safe(display_name(metadata)) ||
        !yaml_scalar_is_safe(metadata.description) || !yaml_scalar_is_safe(installer_url)) {
        return std::unexpected(
            failure(errc::invalid_argument, "winget metadata contains a control character"));
    }
    if (installer.alg != algorithm::sha256) {
        return std::unexpected(
            failure(errc::invalid_argument, "winget installer digest must use SHA-256"));
    }
    if (!installer_url.starts_with("https://")) {
        return std::unexpected(
            failure(errc::invalid_argument, "winget projection requires an HTTPS installer URL"));
    }
    auto architecture = windows_architecture(metadata.architecture);
    if (!architecture) {
        return std::unexpected(architecture.error());
    }
    const auto identifier = package_identifier(metadata);
    if (!winget_identifier_is_valid(identifier)) {
        return std::unexpected(
            failure(errc::invalid_argument, "winget package identifier is invalid", identifier));
    }
    auto version = std::string{};
    version += "PackageIdentifier: " + yaml(identifier) + "\n";
    version += "PackageVersion: " + yaml(metadata.version) + "\n";
    version += "DefaultLocale: en-US\n";
    version += "ManifestType: version\nManifestVersion: 1.9.0\n";
    auto installer_value = std::string{};
    installer_value += "PackageIdentifier: " + yaml(identifier) + "\n";
    installer_value += "PackageVersion: " + yaml(metadata.version) + "\n";
    installer_value += "InstallerType: msix\nInstallers:\n";
    installer_value += "  - Architecture: " + *architecture + "\n";
    installer_value += "    InstallerUrl: " + yaml(installer_url) + "\n";
    installer_value += "    InstallerSha256: " + hex(installer) + "\n";
    installer_value += "ManifestType: installer\nManifestVersion: 1.9.0\n";
    auto locale = std::string{};
    locale += "PackageIdentifier: " + yaml(identifier) + "\n";
    locale += "PackageVersion: " + yaml(metadata.version) + "\n";
    locale += "PackageLocale: en-US\nPublisher: " + yaml(display_name(metadata)) + "\n";
    locale += "PackageName: " + yaml(display_name(metadata)) + "\n";
    locale += "ShortDescription: " +
              yaml(metadata.description.empty() ? display_name(metadata)
                                                : std::string_view{metadata.description}) +
              "\n";
    locale += "ManifestType: defaultLocale\nManifestVersion: 1.9.0\n";
    return std::map<std::string, std::string>{
        {identifier + ".yaml", std::move(version)},
        {identifier + ".installer.yaml", std::move(installer_value)},
        {identifier + ".locale.en-US.yaml", std::move(locale)}};
}

runtime::task<projection_report>
project_macos(repository& source, projection_target target, prepared_projection prepared) {
#if !defined(__APPLE__)
    static_cast<void>(source);
    static_cast<void>(target);
    static_cast<void>(prepared);
    co_return std::unexpected(
        failure(errc::unsupported, "macOS projection requires Apple's installer tools"));
#else
    const auto temporary = temporary_path("macos-projection");
    auto output = co_await runtime::offload(
        [prepared, target, temporary] { return make_macos_artifact(prepared, target, temporary); });
    if (!output || !*output) {
        co_return std::unexpected(output ? output->error() : output.error());
    }
    co_return co_await complete_platform_projection(
        source, target, prepared, temporary, std::move(**output));
#endif
}

runtime::task<projection_report> project_msix(repository& source, prepared_projection prepared) {
#if !defined(_WIN32)
    static_cast<void>(source);
    static_cast<void>(prepared);
    co_return std::unexpected(
        failure(errc::unsupported, "MSIX projection requires makeappx on Windows"));
#else
    const auto temporary = temporary_path("msix-projection");
    auto output = co_await runtime::offload(
        [prepared, temporary] { return make_msix_artifact(prepared, temporary); });
    if (!output || !*output) {
        co_return std::unexpected(output ? output->error() : output.error());
    }
    co_return co_await complete_platform_projection(
        source, projection_target::msix, prepared, temporary, std::move(**output));
#endif
}

runtime::task<projection_report> project_winget(repository& source,
                                                const projection_request& request) {
    if (request.sources.size() != 1) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "winget projection requires one installer path"));
    }
    const auto& path = request.sources.front().path;
    if (path.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "winget source must be an MSIX file"));
    }
    auto installer = co_await runtime::offload([path] { return installer_digest(path); });
    if (!installer) {
        co_return std::unexpected(installer.error());
    }
    if (!*installer) {
        co_return std::unexpected(installer->error());
    }
    auto manifests = winget_manifests(request.metadata, request.options.installer_url, **installer);
    if (!manifests) {
        co_return std::unexpected(manifests.error());
    }
    auto artifacts = std::vector<projected_artifact>{};
    auto combined = std::vector<std::byte>{};
    for (const auto& [name, contents] : *manifests) {
        const auto output = request.options.output_directory / name;
        auto written = co_await runtime::offload(
            [output, contents] { return write_text_file(output, contents); });
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (!*written) {
            co_return std::unexpected(written->error());
        }
        auto digest_value =
            hash(algorithm::sha256,
                 {reinterpret_cast<const std::byte*>(contents.data()), contents.size()});
        if (!digest_value) {
            co_return std::unexpected(digest_value.error());
        }
        artifacts.push_back({output, *digest_value, "application/yaml", true});
        combined.insert(combined.end(),
                        reinterpret_cast<const std::byte*>(name.data()),
                        reinterpret_cast<const std::byte*>(name.data() + name.size()));
        combined.push_back(std::byte{});
        combined.insert(combined.end(),
                        reinterpret_cast<const std::byte*>(contents.data()),
                        reinterpret_cast<const std::byte*>(contents.data() + contents.size()));
    }
    auto subject = hash(algorithm::sha256, combined);
    if (!subject) {
        co_return std::unexpected(subject.error());
    }
    auto prepared = prepared_projection{.metadata = request.metadata,
                                        .options = request.options,
                                        .source_ids = {to_string(**installer)}};
    auto statement = co_await store_projection_attestation(
        source, prepared, projection_target::winget_manifest, *subject);
    if (!statement) {
        co_return std::unexpected(statement.error());
    }
    co_return projection_report{.artifacts = std::move(artifacts),
                                .attestations = {*statement},
                                .pinned = {{request.metadata.name, **installer}}};
}

} // namespace liquid::projection_detail
