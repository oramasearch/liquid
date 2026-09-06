#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "formats/xml/xml.hpp"
#include "liquid/hash.hpp"

namespace liquid::formats::detail {
namespace {

enum class document_kind { ooxml, odf, wheel, jar };

auto kind_name(document_kind kind) -> std::string_view {
    switch (kind) {
    case document_kind::ooxml:
        return "ooxml";
    case document_kind::odf:
        return "odf";
    case document_kind::wheel:
        return "wheel";
    case document_kind::jar:
        return "jar";
    }
    return "zip";
}

auto is_xml(std::string_view path) -> bool {
    return path.ends_with(".xml") || path.ends_with(".rels") || path == "[Content_Types].xml";
}

auto base64url(std::span<const std::byte> input) -> std::string {
    constexpr auto alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    auto output = std::string{};
    for (auto offset = std::size_t{}; offset < input.size(); offset += 3) {
        const auto first = std::to_integer<unsigned char>(input[offset]);
        const auto second =
            offset + 1 < input.size() ? std::to_integer<unsigned char>(input[offset + 1]) : 0;
        const auto third =
            offset + 2 < input.size() ? std::to_integer<unsigned char>(input[offset + 2]) : 0;
        output.push_back(alphabet[first >> 2U]);
        output.push_back(alphabet[((first & 3U) << 4U) | (second >> 4U)]);
        if (offset + 1 < input.size()) {
            output.push_back(alphabet[((second & 15U) << 2U) | (third >> 6U)]);
        }
        if (offset + 2 < input.size()) {
            output.push_back(alphabet[third & 63U]);
        }
    }
    return output;
}

class document_backend final : public codec_backend {
public:
    explicit document_backend(document_kind kind) : kind_(kind) {}

    auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> override {
        const auto extension = std::filesystem::path{path}.extension().string();
        const auto ooxml = extension == ".docx" || extension == ".xlsx" || extension == ".pptx";
        const auto odf = extension == ".odt" || extension == ".ods" || extension == ".odp";
        const auto expected = kind_ == document_kind::ooxml   ? ooxml
                              : kind_ == document_kind::odf   ? odf
                              : kind_ == document_kind::wheel ? extension == ".whl"
                                                              : extension == ".jar";
        if (!expected || blob.size() < 4 || blob[0] != std::byte{'P'} ||
            blob[1] != std::byte{'K'}) {
            return std::optional<format_recognition>{};
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "ZIP members with format-specific canonical entries"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto archive = format_registry::global().unpack(blob, "members.zip", limits);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        auto output = format_tree{.codec = std::string{kind_name(kind_)},
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()},
                                  .truncated = archive->truncated,
                                  .limits = archive->limits};
        for (const auto& source : archive->entries) {
            auto item = source;
            item.nested.reset();
            if ((kind_ == document_kind::ooxml || kind_ == document_kind::odf) &&
                is_xml(item.path) && !item.bytes.empty()) {
                const auto input = std::string_view{
                    reinterpret_cast<const char*>(item.bytes.data()), item.bytes.size()};
                auto canonical = xml::canonicalise(input, kind_ == document_kind::ooxml);
                if (!canonical) {
                    return std::unexpected(canonical.error());
                }
                auto replacement = make_entry(
                    item.path,
                    item.kind,
                    std::vector<std::byte>{
                        reinterpret_cast<const std::byte*>(canonical->data()),
                        reinterpret_cast<const std::byte*>(canonical->data() + canonical->size())},
                    item.attributes,
                    item.source_range);
                if (!replacement) {
                    return std::unexpected(replacement.error());
                }
                item = std::move(*replacement);
            }
            output.entries.push_back(std::move(item));
        }
        if (kind_ == document_kind::wheel) {
            auto found_record = false;
            auto verified = true;
            for (const auto& record : output.entries) {
                if (!record.path.ends_with(".dist-info/RECORD")) {
                    continue;
                }
                found_record = true;
                const auto records = std::string_view{
                    reinterpret_cast<const char*>(record.bytes.data()), record.bytes.size()};
                for (auto line_start = std::size_t{}; line_start < records.size();) {
                    const auto line_end = records.find('\n', line_start);
                    const auto line = records.substr(line_start,
                                                     line_end == std::string_view::npos
                                                         ? records.size() - line_start
                                                         : line_end - line_start);
                    const auto first = line.find(',');
                    const auto second =
                        first == std::string_view::npos ? first : line.find(',', first + 1);
                    if (first != std::string_view::npos && second != std::string_view::npos &&
                        line.substr(first + 1, 7) == "sha256=") {
                        const auto target = std::ranges::find(
                            output.entries, line.substr(0, first), &format_entry::path);
                        auto digest = target == output.entries.end()
                                          ? result<liquid::digest>{std::unexpected(
                                                error{errc::corrupt,
                                                      "wheel RECORD entry is absent",
                                                      std::string{line.substr(0, first)}})}
                                          : liquid::hash(algorithm::sha256, target->bytes);
                        verified =
                            verified && digest &&
                            base64url(digest->bytes) == line.substr(first + 8, second - first - 8);
                    }
                    line_start = line_end == std::string_view::npos ? records.size() : line_end + 1;
                }
            }
            auto attributes =
                detail::attributes({{"record_present", found_record ? "true" : "false"},
                                    {"verified", found_record && verified ? "true" : "false"}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto fact = make_entry(".wheel-record",
                                   format_entry_kind::attribute,
                                   std::vector<std::byte>{},
                                   std::move(*attributes));
            if (!fact) {
                return std::unexpected(fact.error());
            }
            output.entries.push_back(std::move(*fact));
        }
        if (kind_ == document_kind::jar) {
            auto signatures = std::string{};
            for (const auto& item : output.entries) {
                if (item.path.starts_with("META-INF/") &&
                    (item.path.ends_with(".SF") || item.path.ends_with(".RSA") ||
                     item.path.ends_with(".DSA"))) {
                    signatures += item.path + "\n";
                }
            }
            if (!signatures.empty()) {
                auto fact = make_entry(
                    ".jar-signatures",
                    format_entry_kind::attribute,
                    std::vector<std::byte>{
                        reinterpret_cast<const std::byte*>(signatures.data()),
                        reinterpret_cast<const std::byte*>(signatures.data() + signatures.size())});
                if (!fact) {
                    return std::unexpected(fact.error());
                }
                output.entries.push_back(std::move(*fact));
            }
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return fields_;
    }

private:
    document_kind kind_;
    static constexpr auto fields_ =
        std::array<std::string_view, 2>{"relationship-id", "member-order"};
};

template <document_kind Kind>
auto module(std::string_view name, const char* const* extensions, std::size_t count)
    -> const liquid_format_module_v1* {
    static auto backend = document_backend{Kind};
    static auto name_storage = std::string{name};
    static const auto fields = std::array<const char*, 2>{"relationship-id", "member-order"};
    static const auto magic_bytes = std::array<std::uint8_t, 2>{'P', 'K'};
    static const auto magic =
        std::array{liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()}};
    static const auto value =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = name_storage.c_str(),
                                               .tier = 2,
                                               .extensions = extensions,
                                               .extension_count = count,
                                               .magic = magic.data(),
                                               .magic_count = magic.size(),
                                               .volatile_fields = fields.data(),
                                               .volatile_field_count = fields.size(),
                                               .limits = {.maximum_depth = 8,
                                                          .byte_budget = 256ULL * 1024ULL * 1024ULL,
                                                          .maximum_entries = 100'000}},
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

constexpr auto ooxml_extensions = std::array<const char*, 3>{".docx", ".xlsx", ".pptx"};
constexpr auto odf_extensions = std::array<const char*, 3>{".odt", ".ods", ".odp"};
constexpr auto wheel_extensions = std::array<const char*, 1>{".whl"};
constexpr auto jar_extensions = std::array<const char*, 1>{".jar"};

} // namespace

auto ooxml_module() -> const liquid_format_module_v1* {
    return module<document_kind::ooxml>("ooxml", ooxml_extensions.data(), ooxml_extensions.size());
}
auto odf_module() -> const liquid_format_module_v1* {
    return module<document_kind::odf>("odf", odf_extensions.data(), odf_extensions.size());
}
auto wheel_module() -> const liquid_format_module_v1* {
    return module<document_kind::wheel>("wheel", wheel_extensions.data(), wheel_extensions.size());
}
auto jar_module() -> const liquid_format_module_v1* {
    return module<document_kind::jar>("jar", jar_extensions.data(), jar_extensions.size());
}

} // namespace liquid::formats::detail
