// SPDX-License-Identifier: MIT

#include "build/normalise.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>

#include "liquid/hash.hpp"

namespace liquid::build_detail {
namespace {

constexpr auto ar_magic = std::string_view{"!<arch>\n"};
constexpr auto ar_header_size = std::size_t{60};

auto bytes_start_with(std::span<const std::byte> contents, std::string_view prefix) -> bool {
    if (contents.size() < prefix.size()) {
        return false;
    }
    return std::equal(prefix.begin(),
                      prefix.end(),
                      contents.begin(),
                      [](char left, std::byte right) {
                          return static_cast<unsigned char>(left) ==
                                 std::to_integer<unsigned char>(right);
                      });
}

auto byte_at(std::span<const std::byte> contents, std::size_t offset) -> unsigned char {
    return std::to_integer<unsigned char>(contents[offset]);
}

auto is_macho(std::span<const std::byte> contents) -> bool {
    if (contents.size() < 4) {
        return false;
    }
    const auto magic = std::array<unsigned char, 4>{byte_at(contents, 0),
                                                   byte_at(contents, 1),
                                                   byte_at(contents, 2),
                                                   byte_at(contents, 3)};
    return magic == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xce} ||
           magic == std::array<unsigned char, 4>{0xce, 0xfa, 0xed, 0xfe} ||
           magic == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xcf} ||
           magic == std::array<unsigned char, 4>{0xcf, 0xfa, 0xed, 0xfe} ||
           magic == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbe} ||
           magic == std::array<unsigned char, 4>{0xbe, 0xba, 0xfe, 0xca};
}

auto text_extension(std::string extension) -> bool {
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    constexpr auto extensions = std::array<std::string_view, 12>{".c",
                                                                 ".cc",
                                                                 ".cmake",
                                                                 ".cpp",
                                                                 ".h",
                                                                 ".hh",
                                                                 ".hpp",
                                                                 ".inc",
                                                                 ".pc",
                                                                 ".txt",
                                                                 ".in",
                                                                 ".cfg"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

auto field(std::vector<std::byte>& contents, std::size_t offset, std::size_t size)
    -> std::span<std::byte> {
    return std::span<std::byte>{contents}.subspan(offset, size);
}

auto field_text(std::span<const std::byte> value) -> std::string {
    auto output = std::string{};
    output.reserve(value.size());
    for (const auto byte : value) {
        output.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    while (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }
    return output;
}

auto parse_number(std::span<const std::byte> value, int base, std::string_view name)
    -> result<std::uint64_t> {
    const auto text = field_text(value);
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output, base);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::unexpected(
            error{errc::corrupt, "invalid ar member " + std::string{name}, text});
    }
    return output;
}

auto write_number(std::span<std::byte> destination, std::uint64_t value, int base)
    -> result<void> {
    std::ranges::fill(destination, std::byte{' '});
    auto buffer = std::array<char, 32>{};
    const auto encoded = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
    if (encoded.ec != std::errc{} ||
        static_cast<std::size_t>(encoded.ptr - buffer.data()) > destination.size()) {
        return std::unexpected(error{errc::corrupt, "ar member field does not fit"});
    }
    std::transform(buffer.data(),
                   encoded.ptr,
                   destination.begin(),
                   [](char character) { return static_cast<std::byte>(character); });
    return {};
}

auto normalise_ar(std::vector<std::byte> contents) -> result<std::vector<std::byte>> {
    auto offset = ar_magic.size();
    while (offset < contents.size()) {
        if (contents.size() - offset < ar_header_size) {
            return std::unexpected(error{errc::corrupt, "truncated ar member header"});
        }
        const auto header = std::span<const std::byte>{contents}.subspan(offset, ar_header_size);
        if (byte_at(header, 58) != '`' || byte_at(header, 59) != '\n') {
            return std::unexpected(error{errc::corrupt, "invalid ar member trailer"});
        }
        auto size = parse_number(header.subspan(48, 10), 10, "size");
        auto mode = parse_number(header.subspan(40, 8), 8, "mode");
        if (!size || !mode) {
            return std::unexpected(!size ? size.error() : mode.error());
        }
        auto written = write_number(field(contents, offset + 16, 12), 0, 10);
        if (written) {
            written = write_number(field(contents, offset + 28, 6), 0, 10);
        }
        if (written) {
            written = write_number(field(contents, offset + 34, 6), 0, 10);
        }
        if (written) {
            written = write_number(field(contents, offset + 40, 8),
                                   normalised_output_mode(static_cast<std::uint32_t>(*mode)),
                                   8);
        }
        if (!written) {
            return std::unexpected(written.error());
        }
        offset += ar_header_size;
        if (*size > contents.size() - offset) {
            return std::unexpected(error{errc::corrupt, "truncated ar member contents"});
        }
        offset += static_cast<std::size_t>(*size);
        if ((offset & 1U) != 0U) {
            ++offset;
        }
    }
    if (offset != contents.size()) {
        return std::unexpected(error{errc::corrupt, "invalid ar archive padding"});
    }
    return contents;
}

auto normalise_pyc(std::vector<std::byte> contents) -> result<std::vector<std::byte>> {
    constexpr auto header_size = std::size_t{16};
    if (contents.size() < header_size) {
        return std::unexpected(error{errc::corrupt, "truncated Python bytecode header"});
    }
    contents[4] = std::byte{1};
    contents[5] = std::byte{};
    contents[6] = std::byte{};
    contents[7] = std::byte{};
    auto payload = std::span<const std::byte>{contents}.subspan(header_size);
    auto payload_hash = hash(algorithm::blake3, payload);
    if (!payload_hash) {
        return std::unexpected(payload_hash.error());
    }
    std::copy_n(payload_hash->bytes.begin(), 8, contents.begin() + 8);
    return contents;
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            error{errc::io, "cannot read output for normalisation", path.string()});
    }
    auto bytes = std::vector<char>{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return std::unexpected(
            error{errc::io, "cannot read output for normalisation", path.string()});
    }
    auto output = std::vector<std::byte>(bytes.size());
    std::transform(bytes.begin(), bytes.end(), output.begin(), [](char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return output;
}

auto write_file(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected(
            error{errc::io, "cannot write normalised output", path.string()});
    }
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        return std::unexpected(
            error{errc::io, "cannot write normalised output", path.string()});
    }
    return {};
}

} // namespace

auto output_kind_for(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> output_kind {
    if (bytes_start_with(contents, ar_magic)) {
        return output_kind::ar_archive;
    }
    if (bytes_start_with(contents, std::string_view{"\x7f" "ELF", 4})) {
        return output_kind::elf_object;
    }
    if (is_macho(contents)) {
        return output_kind::macho_object;
    }
    if (bytes_start_with(contents, "MZ")) {
        return output_kind::pe_object;
    }
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".pyc") {
        return output_kind::python_bytecode;
    }
    if (text_extension(extension) || path.filename().string().ends_with("Config.cmake")) {
        return output_kind::text;
    }
    return output_kind::opaque;
}

auto normalise_output(output_kind kind, std::vector<std::byte> contents)
    -> result<std::vector<std::byte>> {
    if (kind == output_kind::ar_archive) {
        return normalise_ar(std::move(contents));
    }
    if (kind == output_kind::python_bytecode) {
        return normalise_pyc(std::move(contents));
    }
    return contents;
}

auto normalise_file(const std::filesystem::path& path) -> result<bool> {
    auto original = read_file(path);
    if (!original) {
        return std::unexpected(original.error());
    }
    auto normalised = normalise_output(output_kind_for(path, *original), *original);
    if (!normalised) {
        return std::unexpected(normalised.error());
    }
    if (*normalised == *original) {
        return false;
    }
    auto written = write_file(path, *normalised);
    if (!written) {
        return std::unexpected(written.error());
    }
    return true;
}

auto normalised_output_mode(std::uint32_t mode) -> std::uint32_t {
    return (mode & 0111U) != 0U ? 0755U : 0644U;
}

auto output_normaliser_rules() -> std::vector<std::string> {
    return {"tree-mode-v1", "gnu-ar-d-v1", "python-hash-pyc-v1", "stable-path-binary-v1",
            "source-date-epoch-text-v1"};
}

} // namespace liquid::build_detail
