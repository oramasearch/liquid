#include "formats/tar/writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace liquid::formats::tar {
namespace {

constexpr auto block_size = std::size_t{512};

void write_text(std::span<std::byte> field, std::string_view value) {
    const auto length = std::min(field.size(), value.size());
    for (auto index = std::size_t{}; index < length; ++index) {
        field[index] = std::byte{static_cast<unsigned char>(value[index])};
    }
}

auto write_octal(std::span<std::byte> field, std::uint64_t value) -> result<void> {
    auto text = std::array<char, 32>{};
    const auto [end, code] = std::to_chars(text.data(), text.data() + text.size(), value, 8);
    if (code != std::errc{} || static_cast<std::size_t>(end - text.data()) + 1 > field.size()) {
        return std::unexpected(error{errc::unsupported, "tar numeric field is too large", "tar"});
    }
    std::ranges::fill(field, std::byte{'0'});
    const auto digits = static_cast<std::size_t>(end - text.data());
    for (auto index = std::size_t{}; index < digits; ++index) {
        field[field.size() - digits - 1 + index] =
            std::byte{static_cast<unsigned char>(text[index])};
    }
    field.back() = std::byte{};
    return {};
}

auto pax_record(std::string_view key, std::string_view value) -> std::string {
    auto length = key.size() + value.size() + 3;
    while (true) {
        const auto digits = std::to_string(length).size();
        const auto actual = digits + 1 + key.size() + 1 + value.size() + 1;
        if (actual == length) {
            return std::to_string(length) + " " + std::string{key} + "=" + std::string{value} +
                   "\n";
        }
        length = actual;
    }
}

auto append_member(std::vector<std::byte>& output,
                   std::string_view name,
                   char type,
                   std::span<const std::byte> contents,
                   std::uint64_t mode,
                   std::uint64_t mtime) -> result<void> {
    auto header = std::array<std::byte, block_size>{};
    write_text(std::span{header}.subspan(0, 100), name);
    auto mode_result = write_octal(std::span{header}.subspan(100, 8), mode);
    auto uid_result = write_octal(std::span{header}.subspan(108, 8), 0);
    auto gid_result = write_octal(std::span{header}.subspan(116, 8), 0);
    auto size_result = write_octal(std::span{header}.subspan(124, 12), contents.size());
    auto time_result = write_octal(std::span{header}.subspan(136, 12), mtime);
    if (!mode_result || !uid_result || !gid_result || !size_result || !time_result) {
        return std::unexpected(
            error{errc::unsupported, "tar header value is unsupported", std::string{name}});
    }
    std::ranges::fill(std::span{header}.subspan(148, 8), std::byte{' '});
    header[156] = std::byte{static_cast<unsigned char>(type)};
    write_text(std::span{header}.subspan(257, 6), "ustar");
    write_text(std::span{header}.subspan(263, 2), "00");
    auto checksum = std::uint64_t{};
    for (const auto value : header) {
        checksum += std::to_integer<unsigned char>(value);
    }
    auto checksum_result = write_octal(std::span{header}.subspan(148, 7), checksum);
    if (!checksum_result) {
        return std::unexpected(checksum_result.error());
    }
    header[154] = std::byte{};
    header[155] = std::byte{' '};
    output.insert(output.end(), header.begin(), header.end());
    output.insert(output.end(), contents.begin(), contents.end());
    const auto remainder = contents.size() % block_size;
    if (remainder != 0) {
        output.resize(output.size() + block_size - remainder);
    }
    return {};
}

} // namespace

auto write(std::span<const entry> entries, std::uint64_t source_date_epoch)
    -> result<std::vector<std::byte>> {
    auto sorted = std::vector<entry>{entries.begin(), entries.end()};
    std::ranges::sort(sorted, {}, &entry::path);
    auto output = std::vector<std::byte>{};
    for (const auto& entry : sorted) {
        auto name = entry.path;
        const auto directory = entry.directory;
        if (directory && !name.ends_with('/')) {
            name.push_back('/');
        }
        if (name.size() > 100) {
            const auto record = pax_record("path", name);
            const auto record_bytes =
                std::span{reinterpret_cast<const std::byte*>(record.data()), record.size()};
            auto pax = append_member(
                output, "PaxHeaders/liquid", 'x', record_bytes, 0644, source_date_epoch);
            if (!pax) {
                return std::unexpected(pax.error());
            }
        }
        const auto header_name = name.size() <= 100 ? name : name.substr(0, 100);
        const auto mode = directory ? 0755U : entry.mode;
        auto appended = append_member(output,
                                      header_name,
                                      directory ? '5' : '0',
                                      directory ? std::span<const std::byte>{}
                                                : std::span<const std::byte>{entry.bytes},
                                      mode,
                                      source_date_epoch);
        if (!appended) {
            return std::unexpected(appended.error());
        }
    }
    output.resize(output.size() + block_size * 2);
    return output;
}

} // namespace liquid::formats::tar
