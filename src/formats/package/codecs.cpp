// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <ranges>

#include "formats/apk/apk.hpp"
#include "formats/deb/deb.hpp"
#include "formats/rpm/rpm.hpp"

namespace liquid::formats::detail {
namespace {

enum class package_kind { deb, rpm, apk };

class package_backend final : public codec_backend {
public:
    explicit package_backend(package_kind kind) : kind_(kind) {}

    auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> override {
        const auto extension = std::filesystem::path{path}.extension().string();
        const auto magic =
            kind_ == package_kind::deb
                ? blob.size() >= 8 &&
                      std::string_view{reinterpret_cast<const char*>(blob.data()), 8} == "!<arch>\n"
            : kind_ == package_kind::rpm
                ? blob.size() >= 4 && blob[0] == std::byte{0xed} && blob[1] == std::byte{0xab} &&
                      blob[2] == std::byte{0xee} && blob[3] == std::byte{0xdb}
                : blob.size() >= 2 && blob[0] == std::byte{0x1f} && blob[1] == std::byte{0x8b} &&
                      extension == ".apk";
        if (!magic) {
            return std::optional<format_recognition>{};
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "package metadata and payload are decoded"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto output = format_tree{.codec = kind_ == package_kind::deb   ? "deb"
                                           : kind_ == package_kind::rpm ? "rpm"
                                                                        : "apk",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        auto metadata = std::string{};
        auto files = std::vector<std::string>{};
        auto scripts = std::optional<generated_maintainer_scripts>{};
        auto control_entries = std::vector<format_entry>{};
        auto data_entries = std::vector<format_entry>{};
        auto data_files = std::vector<package::file>{};
        if (kind_ == package_kind::deb) {
            auto value = deb::read(blob, limits.byte_budget);
            if (!value) {
                return std::unexpected(value.error());
            }
            metadata = std::move(value->control);
            files = std::move(value->files);
            scripts = std::move(value->scripts);
            control_entries = std::move(value->control_entries);
            data_entries = std::move(value->data_entries);
        } else if (kind_ == package_kind::rpm) {
            auto value = rpm::read(blob, limits.byte_budget);
            if (!value) {
                return std::unexpected(value.error());
            }
            metadata = "Name: " + value->name + "\nVersion: " + value->version + "\n";
            files = std::move(value->files);
            scripts = std::move(value->scripts);
            data_files = std::move(value->data_files);
        } else {
            auto value = apk::read(blob, limits.byte_budget);
            if (!value) {
                return std::unexpected(value.error());
            }
            metadata = std::move(value->pkginfo);
            files = std::move(value->files);
            scripts = std::move(value->scripts);
            control_entries = std::move(value->control_entries);
            data_entries = std::move(value->data_entries);
        }
        const auto decoded_entries =
            control_entries.size() + data_entries.size() + data_files.size();
        const auto output_entries = decoded_entries == 0 ? files.size() : decoded_entries;
        const auto script_entries = scripts ? std::size_t{4} : std::size_t{};
        if (output_entries + 1 + script_entries > limits.maximum_entries) {
            return std::unexpected(
                error{errc::unsupported, "package entry limit exceeded", output.codec});
        }
        auto metadata_entry =
            make_entry(".metadata", format_entry_kind::attribute, package::text_bytes(metadata));
        if (!metadata_entry) {
            return std::unexpected(metadata_entry.error());
        }
        output.entries.push_back(std::move(*metadata_entry));
        const auto has_payload_entries = !data_entries.empty() || !data_files.empty();
        for (auto& item : control_entries) {
            item.path = "control/" + item.path;
            item.nested.reset();
            output.entries.push_back(std::move(item));
        }
        for (auto& item : data_entries) {
            item.path = "data/" + item.path;
            item.nested.reset();
            output.entries.push_back(std::move(item));
        }
        for (auto& file : data_files) {
            auto entry = make_entry(
                "data/" + std::move(file.path), format_entry_kind::member, std::move(file.bytes));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        for (auto& path : files) {
            if (has_payload_entries) {
                continue;
            }
            auto entry = make_entry(std::move(path), format_entry_kind::member, {});
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        if (scripts) {
            const auto names =
                kind_ == package_kind::deb
                    ? std::array<std::string_view, 4>{"preinst", "postinst", "prerm", "postrm"}
                : kind_ == package_kind::rpm
                    ? std::array<std::string_view, 4>{"%pre", "%post", "%preun", "%postun"}
                    : std::array<std::string_view, 4>{
                          ".pre-install", ".post-install", ".pre-deinstall", ".post-deinstall"};
            const auto values = std::array<std::string_view, 4>{scripts->pre_install,
                                                                scripts->post_install,
                                                                scripts->pre_remove,
                                                                scripts->post_remove};
            for (std::size_t index = 0; index < names.size(); ++index) {
                auto entry = make_entry(".scripts/" + std::string{names[index]},
                                        format_entry_kind::member,
                                        package::text_bytes(values[index]));
                if (!entry) {
                    return std::unexpected(entry.error());
                }
                output.entries.push_back(std::move(*entry));
            }
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return {};
    }

private:
    package_kind kind_;
};

template <package_kind Kind>
auto module(std::string_view name,
            const char* const* extensions,
            std::size_t extension_count,
            const liquid_format_magic_v1* magic,
            std::size_t magic_count) -> const liquid_format_module_v1* {
    static auto backend = package_backend{Kind};
    static auto name_storage = std::string{name};
    static const auto value =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = name_storage.c_str(),
                                               .tier = 2,
                                               .extensions = extensions,
                                               .extension_count = extension_count,
                                               .magic = magic,
                                               .magic_count = magic_count,
                                               .volatile_fields = nullptr,
                                               .volatile_field_count = 0,
                                               .limits = {.maximum_depth = 1,
                                                          .byte_budget = 512ULL * 1024ULL * 1024ULL,
                                                          .maximum_entries = 1'000'000}},
                                .module_context = &backend,
                                .recognise = recognise_operation,
                                .unpack = unpack_operation,
                                .canonicalise = canonicalise_operation,
                                .diff = diff_operation,
                                .merge = nullptr,
                                .repack = nullptr,
                                .destroy = nullptr};
    return &value;
}

constexpr auto deb_extensions = std::array<const char*, 1>{".deb"};
constexpr auto rpm_extensions = std::array<const char*, 1>{".rpm"};
constexpr auto apk_extensions = std::array<const char*, 1>{".apk"};
constexpr auto deb_magic_bytes =
    std::array<std::uint8_t, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
constexpr auto rpm_magic_bytes = std::array<std::uint8_t, 4>{0xed, 0xab, 0xee, 0xdb};
constexpr auto gzip_magic_bytes = std::array<std::uint8_t, 2>{0x1f, 0x8b};
constexpr auto deb_magic =
    std::array{liquid_format_magic_v1{deb_magic_bytes.data(), deb_magic_bytes.size()}};
constexpr auto rpm_magic =
    std::array{liquid_format_magic_v1{rpm_magic_bytes.data(), rpm_magic_bytes.size()}};
constexpr auto apk_magic =
    std::array{liquid_format_magic_v1{gzip_magic_bytes.data(), gzip_magic_bytes.size()}};

} // namespace

auto deb_module() -> const liquid_format_module_v1* {
    return module<package_kind::deb>(
        "deb", deb_extensions.data(), deb_extensions.size(), deb_magic.data(), deb_magic.size());
}

auto rpm_module() -> const liquid_format_module_v1* {
    return module<package_kind::rpm>(
        "rpm", rpm_extensions.data(), rpm_extensions.size(), rpm_magic.data(), rpm_magic.size());
}

auto apk_module() -> const liquid_format_module_v1* {
    return module<package_kind::apk>(
        "apk", apk_extensions.data(), apk_extensions.size(), apk_magic.data(), apk_magic.size());
}

} // namespace liquid::formats::detail
