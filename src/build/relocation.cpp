#include "build/relocation.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <set>
#include <string_view>

#include "build/repository_access.hpp"
#include "formats/binary/elf.hpp"
#include "formats/binary/macho.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

constexpr auto virtual_root = std::string_view{"/liquid/work"};

auto relocation_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto byte_string(std::span<const std::byte> bytes) -> std::string_view {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto printable_context(std::string_view text, std::size_t offset, std::size_t length)
    -> std::string {
    constexpr auto radius = std::size_t{24};
    const auto begin = offset > radius ? offset - radius : 0;
    const auto end = std::min(text.size(), offset + length + radius);
    auto output = std::string{text.substr(begin, end - begin)};
    for (auto& character : output) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isprint(value)) {
            character = '.';
        }
    }
    return output;
}

void add_finding(relocation_report& report,
                 std::string_view file,
                 std::uint64_t offset,
                 std::string context) {
    report.findings.push_back({std::string{file}, offset, std::move(context)});
    report.relocatable = false;
}

void scan_embedded_prefixes(relocation_report& report,
                            const build_detail::relocation_file& file,
                            const std::optional<std::filesystem::path>& install_prefix) {
    auto prefixes = std::vector<std::pair<std::string, std::string>>{};
    if (install_prefix) {
        const auto prefix = install_prefix->generic_string();
        if (prefix.size() > 1) {
            prefixes.emplace_back(prefix, "declared install prefix");
        }
    }
    prefixes.emplace_back(virtual_root, "virtual build root");
    std::ranges::sort(prefixes, [](const auto& left, const auto& right) {
        return left.first.size() > right.first.size();
    });
    const auto text = byte_string(file.contents);
    auto recorded = std::set<std::size_t>{};
    for (const auto& [prefix, description] : prefixes) {
        auto offset = text.find(prefix);
        while (offset != std::string_view::npos) {
            if (recorded.insert(offset).second) {
                add_finding(report,
                            file.path,
                            offset,
                            description + ": " + printable_context(text, offset, prefix.size()));
            }
            offset = text.find(prefix, offset + 1);
        }
    }
}

auto absolute_path(std::string_view value) -> bool {
    return value.starts_with('/');
}

auto absolute_path_offset(std::string_view line) -> std::optional<std::size_t> {
    auto offset = line.find('/');
    while (offset != std::string_view::npos) {
        const auto url = offset > 0 && offset + 1 < line.size() && line[offset - 1] == ':' &&
                         line[offset + 1] == '/';
        const auto previous = offset == 0 ? '\0' : line[offset - 1];
        const auto flag = offset >= 2 && (line.substr(offset - 2, 2) == "-L" ||
                                          line.substr(offset - 2, 2) == "-I");
        const auto boundary = offset == 0 || std::isspace(static_cast<unsigned char>(previous)) ||
                              previous == '"' || previous == '\'' || previous == '=' ||
                              previous == ',' || previous == ':' || flag;
        if (!url && boundary) {
            return offset;
        }
        offset = line.find('/', offset + 1);
    }
    return std::nullopt;
}

void scan_elf_paths(relocation_report& report,
                    const build_detail::relocation_file& file,
                    const formats::binary::image& image) {
    for (const auto& reference : image.path_references) {
        if (reference.kind == formats::binary::path_kind::runtime_search_path) {
            auto begin = std::size_t{};
            while (begin <= reference.value.size()) {
                const auto end = reference.value.find(':', begin);
                const auto entry = std::string_view{reference.value}.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin);
                if (absolute_path(entry)) {
                    add_finding(report,
                                file.path,
                                reference.offset + begin,
                                "absolute ELF RPATH/RUNPATH: " + std::string{entry});
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
            continue;
        }
        if (absolute_path(reference.value)) {
            const auto description = reference.kind == formats::binary::path_kind::needed_library
                                         ? "absolute ELF DT_NEEDED: "
                                         : "absolute ELF install name: ";
            add_finding(report, file.path, reference.offset, description + reference.value);
        }
    }
}

void scan_macho_paths(relocation_report& report,
                      const build_detail::relocation_file& file,
                      const formats::binary::image& image) {
    for (const auto& reference : image.path_references) {
        if (reference.kind == formats::binary::path_kind::needed_library ||
            !absolute_path(reference.value)) {
            continue;
        }
        const auto description = reference.kind == formats::binary::path_kind::runtime_search_path
                                     ? "absolute Mach-O LC_RPATH: "
                                     : "absolute Mach-O install name: ";
        add_finding(report, file.path, reference.offset, description + reference.value);
    }
}

void scan_binary_paths(relocation_report& report, const build_detail::relocation_file& file) {
    const auto bytes = std::span<const std::byte>{file.contents};
    if (bytes.size() >= 4 && bytes[0] == std::byte{0x7f} && bytes[1] == std::byte{'E'} &&
        bytes[2] == std::byte{'L'} && bytes[3] == std::byte{'F'}) {
        auto image = formats::binary::inspect_elf(bytes);
        if (image) {
            if (image->truncated) {
                add_finding(report, file.path, 0, "ELF symbol inspection was truncated");
            }
            scan_elf_paths(report, file, *image);
        }
        return;
    }
    if (bytes.size() < 4) {
        return;
    }
    const auto first = std::to_integer<unsigned char>(bytes[0]);
    const auto second = std::to_integer<unsigned char>(bytes[1]);
    const auto third = std::to_integer<unsigned char>(bytes[2]);
    const auto fourth = std::to_integer<unsigned char>(bytes[3]);
    const auto macho =
        (first == 0xfe && second == 0xed && third == 0xfa && (fourth == 0xce || fourth == 0xcf)) ||
        ((first == 0xce || first == 0xcf) && second == 0xfa && third == 0xed && fourth == 0xfe);
    if (!macho) {
        return;
    }
    auto image = formats::binary::inspect_macho(bytes);
    if (image) {
        if (image->truncated) {
            add_finding(report, file.path, 0, "Mach-O symbol inspection was truncated");
        }
        scan_macho_paths(report, file, *image);
    }
}

void scan_pc_file(relocation_report& report, const build_detail::relocation_file& file) {
    const auto text = byte_string(file.contents);
    auto begin = std::size_t{};
    while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = text.substr(begin, end - begin);
        const auto path = absolute_path_offset(line);
        if (path) {
            add_finding(
                report, file.path, begin + *path, "absolute pkg-config path: " + std::string{line});
        }
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
    }
}

void scan_cmake_file(relocation_report& report, const build_detail::relocation_file& file) {
    const auto text = byte_string(file.contents);
    auto begin = std::size_t{};
    while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = text.substr(begin, end - begin);
        const auto path = absolute_path_offset(line);
        if (path) {
            const auto value_end = line.find_first_of("\"' )\t", *path);
            const auto value = line.substr(*path, value_end - *path);
            add_finding(report,
                        file.path,
                        begin + *path,
                        "absolute CMake package path: " + std::string{value});
        }
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
    }
}

void scan_script(relocation_report& report, const build_detail::relocation_file& file) {
    const auto text = byte_string(file.contents);
    if (!text.starts_with("#!/") || text.starts_with("#!/usr/bin/env ")) {
        return;
    }
    const auto end = text.find('\n');
    add_finding(
        report, file.path, 0, "absolute script interpreter: " + std::string{text.substr(0, end)});
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(
            relocation_error(errc::io, "cannot open relocation input", path.string()));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(
            relocation_error(errc::io, "cannot size relocation input", path.string()));
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input) {
        return std::unexpected(
            relocation_error(errc::io, "cannot read relocation input", path.string()));
    }
    return output;
}

auto load_blob_contents(repository& source, digest blob_id)
    -> runtime::task<std::vector<std::byte>> {
    auto record_bytes = co_await repository_detail::build_access::get(source, blob_id);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    auto record = objects::decode_blob(*record_bytes);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    if (record->remote) {
        co_return std::unexpected(relocation_error(
            errc::unsupported, "relocation check requires a local output", to_string(blob_id)));
    }
    if (record->content.alg == algorithm::blake3 || record->content.alg == algorithm::sha256 ||
        record->content.alg == algorithm::blake3_keyed) {
        co_return co_await repository_detail::build_access::get(source, record->content);
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(record->size));
    for (const auto& chunk : record->chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != record->size) {
        co_return std::unexpected(relocation_error(
            errc::corrupt, "relocation input length differs from blob", to_string(blob_id)));
    }
    co_return output;
}

} // namespace

namespace build_detail {

auto inspect_relocation(std::span<const relocation_file> files,
                        std::optional<std::filesystem::path> install_prefix)
    -> result<relocation_report> {
    auto report = relocation_report{};
    for (const auto& file : files) {
        scan_embedded_prefixes(report, file, install_prefix);
        scan_binary_paths(report, file);
        const auto path = std::filesystem::path{file.path};
        if (path.extension() == ".pc") {
            scan_pc_file(report, file);
        }
        if (path.extension() == ".cmake") {
            scan_cmake_file(report, file);
        }
        scan_script(report, file);
    }
    std::ranges::sort(report.findings, [](const auto& left, const auto& right) {
        return std::pair{left.file, left.offset} < std::pair{right.file, right.offset};
    });
    return report;
}

auto relocation_check(repository& source,
                      std::span<const action_output> outputs,
                      std::optional<std::filesystem::path> install_prefix)
    -> runtime::task<relocation_report> {
    auto files = std::vector<relocation_file>{};
    files.reserve(outputs.size());
    for (const auto& output : outputs) {
        auto contents = co_await load_blob_contents(source, output.blob);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        files.push_back({output.path, std::move(*contents)});
    }
    auto checked = co_await runtime::offload(
        [files = std::move(files), install_prefix = std::move(install_prefix)] {
            return inspect_relocation(files, install_prefix);
        });
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    co_return std::move(*checked);
}

auto relocation_failure(const relocation_report& report) -> error {
    auto message = std::string{"relocation check failed"};
    const auto count = std::min<std::size_t>(report.findings.size(), 3);
    for (auto index = std::size_t{}; index < count; ++index) {
        const auto& finding = report.findings[index];
        message += index == 0 ? ": " : "; ";
        message += finding.file + "@" + std::to_string(finding.offset) + " " + finding.context;
    }
    return relocation_error(errc::invalid_argument, std::move(message), "install output tree");
}

} // namespace build_detail

namespace build {

runtime::task<relocation_report> relocation_check(std::filesystem::path output_tree,
                                                  relocation_check_options options) {
    auto checked = co_await runtime::offload(
        [output_tree = std::move(output_tree),
         options = std::move(options)]() mutable -> liquid::result<relocation_report> {
            auto code = std::error_code{};
            if (!std::filesystem::is_directory(output_tree, code) || code) {
                return std::unexpected(relocation_error(errc::invalid_argument,
                                                        "relocation output is not a directory",
                                                        output_tree.string()));
            }
            auto files = std::vector<build_detail::relocation_file>{};
            for (auto iterator = std::filesystem::recursive_directory_iterator{output_tree, code};
                 !code && iterator != std::filesystem::recursive_directory_iterator{};
                 iterator.increment(code)) {
                if (!iterator->is_regular_file(code)) {
                    continue;
                }
                auto contents = read_file(iterator->path());
                if (!contents) {
                    return std::unexpected(contents.error());
                }
                files.push_back({iterator->path().lexically_relative(output_tree).generic_string(),
                                 std::move(*contents)});
            }
            if (code) {
                return std::unexpected(
                    relocation_error(errc::io, code.message(), output_tree.string()));
            }
            std::ranges::sort(files, {}, &build_detail::relocation_file::path);
            return build_detail::inspect_relocation(files, std::move(options.install_prefix));
        });
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    co_return std::move(*checked);
}

} // namespace build
} // namespace liquid
