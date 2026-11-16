// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace liquid::formats::detail {
namespace {

auto text(std::span<const std::byte> bytes) -> std::string_view {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto inflate(std::span<const std::byte> input, std::uint64_t limit)
    -> result<std::vector<std::byte>> {
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit(&stream) != Z_OK) {
        return std::unexpected(error{errc::corrupt, "cannot initialise PDF FlateDecode", "pdf"});
    }
    auto output = std::vector<std::byte>{};
    auto chunk = std::array<std::byte, 16 * 1024>{};
    auto status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = ::inflate(&stream, Z_NO_FLUSH);
        const auto written = chunk.size() - stream.avail_out;
        if (written > limit - std::min<std::uint64_t>(limit, output.size())) {
            inflateEnd(&stream);
            return std::unexpected(
                error{errc::unsupported, "PDF stream exceeds byte budget", "pdf"});
        }
        output.insert(
            output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(written));
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return std::unexpected(error{errc::corrupt, "invalid PDF FlateDecode stream", "pdf"});
    }
    return output;
}

auto unsigned_value(std::string_view input, std::string_view key) -> std::optional<std::size_t> {
    const auto at = input.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    auto value = std::size_t{};
    const auto start = at + key.size();
    const auto [end, code] =
        std::from_chars(input.data() + start, input.data() + input.size(), value);
    return code == std::errc{} && end != input.data() + start ? std::optional{value} : std::nullopt;
}

auto metadata_value(std::string_view input, std::string_view key) -> std::optional<std::string> {
    const auto start = input.find(key);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value_start = start + key.size();
    const auto value_end = input.find(')', value_start);
    if (value_end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string{input.substr(value_start, value_end - value_start)};
}

auto normalise(std::string value) -> std::string {
    for (const auto key : {"/ID", "/CreationDate", "/ModDate", "/Producer"}) {
        auto at = value.find(key);
        while (at != std::string::npos) {
            const auto key_size = std::string_view{key}.size();
            const auto end = value.find_first_of(")>]", at + key_size);
            value.replace(at,
                          (end == std::string::npos ? value.size() : end + 1) - at,
                          std::string{key} + " <volatile>");
            at = value.find(key, at + 1);
        }
    }
    return value;
}

// Extracts the literal strings shown by text operators, failing before the text passes `limit`
// bytes.
auto content_text(std::string_view content, std::uint64_t limit) -> result<std::string> {
    auto output = std::string{};
    for (auto position = std::size_t{}; position < content.size(); ++position) {
        if (content[position] != '(') {
            continue;
        }
        auto literal = std::string{};
        auto escaped = false;
        ++position;
        for (; position < content.size(); ++position) {
            const auto value = content[position];
            if (!escaped && value == ')') {
                break;
            }
            if (!escaped && value == '\\') {
                escaped = true;
                continue;
            }
            literal.push_back(value);
            escaped = false;
        }
        auto tail = content.substr(std::min(content.size(), position + 1));
        const auto operator_start = tail.find_first_not_of(" \t\r\n");
        const auto operator_tail = operator_start == std::string_view::npos
                                       ? std::string_view{}
                                       : tail.substr(operator_start);
        if (operator_tail.starts_with("Tj") || operator_tail.starts_with("TJ") ||
            operator_tail.starts_with("'") || operator_tail.starts_with("\"")) {
            if (literal.size() + 1 > limit - std::min<std::uint64_t>(limit, output.size())) {
                return std::unexpected(
                    error{errc::unsupported, "PDF text exceeds byte budget", "pdf"});
            }
            output += literal;
            output.push_back('\n');
        }
    }
    return output;
}

class pdf_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (!text(blob).starts_with("%PDF-")) {
            return std::optional<format_recognition>{};
        }
        if (text(blob).find("/Encrypt") != std::string_view::npos) {
            return std::unexpected(error{errc::unsupported, "encrypted PDF is unsupported", "pdf"});
        }
        return std::optional<format_recognition>{
            format_recognition{format_tier::semantic_verbatim, "PDF objects and text streams"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        const auto source = text(blob);
        if (!source.starts_with("%PDF-")) {
            return std::unexpected(error{errc::corrupt, "invalid PDF header", "pdf"});
        }
        if (source.find("/Encrypt") != std::string_view::npos) {
            return std::unexpected(error{errc::unsupported, "encrypted PDF is unsupported", "pdf"});
        }
        auto output = format_tree{.codec = "pdf",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        auto offset = std::size_t{};
        auto remaining = limits.byte_budget;
        while (offset < source.size()) {
            const auto marker = source.find(" obj", offset);
            if (marker == std::string_view::npos) {
                break;
            }
            const auto line = source.rfind('\n', marker);
            const auto header =
                source.substr(line == std::string_view::npos ? 0 : line + 1,
                              marker - (line == std::string_view::npos ? 0 : line + 1));
            const auto end = source.find("endobj", marker + 4);
            if (end == std::string_view::npos) {
                return std::unexpected(error{errc::corrupt, "unterminated PDF object", "pdf"});
            }
            const auto object_end = end + 6;
            if (object_end - (line == std::string_view::npos ? 0 : line + 1) > remaining) {
                output.truncated = true;
                output.limits.push_back("byte budget reached at object " + std::string{header});
                break;
            }
            remaining -= object_end - (line == std::string_view::npos ? 0 : line + 1);
            auto body = source.substr(marker + 4, end - (marker + 4));
            auto canonical = normalise(std::string{body});
            auto entry = make_entry(
                "objects/" + std::string{header},
                format_entry_kind::member,
                std::vector<std::byte>{
                    reinterpret_cast<const std::byte*>(canonical.data()),
                    reinterpret_cast<const std::byte*>(canonical.data() + canonical.size())});
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
            for (const auto key : {"/Title (", "/Author (", "/Subject (", "/Keywords ("}) {
                const auto value = metadata_value(body, key);
                if (!value) {
                    continue;
                }
                auto metadata = make_entry(
                    "metadata/" + std::string{key + 1, key + 1 + std::string_view{key}.find(' ')},
                    format_entry_kind::attribute,
                    std::vector<std::byte>{
                        reinterpret_cast<const std::byte*>(value->data()),
                        reinterpret_cast<const std::byte*>(value->data() + value->size())});
                if (!metadata) {
                    return std::unexpected(metadata.error());
                }
                output.entries.push_back(std::move(*metadata));
            }
            const auto stream = body.find("stream");
            const auto stream_end =
                stream == std::string_view::npos ? stream : body.find("endstream", stream + 6);
            if (stream != std::string_view::npos && stream_end != std::string_view::npos) {
                auto start = stream + 6;
                if (start < body.size() && body[start] == '\r') {
                    ++start;
                }
                if (start < body.size() && body[start] == '\n') {
                    ++start;
                }
                auto content = body.substr(start, stream_end - start);
                // Inflated bytes and the text taken from them both stay charged, so streams
                // cannot each inflate against the same allowance.
                auto decoded = std::vector<std::byte>{};
                if (body.find("/FlateDecode") != std::string_view::npos) {
                    auto inflated = inflate(std::as_bytes(std::span{content}), remaining);
                    if (!inflated) {
                        return std::unexpected(inflated.error());
                    }
                    decoded = std::move(*inflated);
                    remaining -= decoded.size();
                    content = text(decoded);
                }
                auto extracted = content_text(content, remaining);
                if (!extracted) {
                    return std::unexpected(extracted.error());
                }
                remaining -= extracted->size();
                if (!extracted->empty()) {
                    auto text_entry =
                        make_entry("text/" + std::string{header},
                                   format_entry_kind::member,
                                   std::vector<std::byte>{
                                       reinterpret_cast<const std::byte*>(extracted->data()),
                                       reinterpret_cast<const std::byte*>(extracted->data() +
                                                                          extracted->size())});
                    if (!text_entry) {
                        return std::unexpected(text_entry.error());
                    }
                    output.entries.push_back(std::move(*text_entry));
                }
            }
            offset = object_end;
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return fields_;
    }

private:
    static constexpr auto fields_ =
        std::array<std::string_view, 4>{"/ID", "/CreationDate", "/ModDate", "/Producer"};
};

constexpr auto extensions = std::array<const char*, 1>{".pdf"};
constexpr auto magic_bytes = std::array<std::uint8_t, 5>{'%', 'P', 'D', 'F', '-'};
constexpr auto magic = std::array{liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()}};
constexpr auto fields = std::array<const char*, 4>{"/ID", "/CreationDate", "/ModDate", "/Producer"};

} // namespace

auto pdf_module() -> const liquid_format_module_v1* {
    static auto backend = pdf_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "pdf",
                                               .tier = 2,
                                               .extensions = extensions.data(),
                                               .extension_count = extensions.size(),
                                               .magic = magic.data(),
                                               .magic_count = magic.size(),
                                               .volatile_fields = fields.data(),
                                               .volatile_field_count = fields.size(),
                                               .limits = {.maximum_depth = 1,
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
    return &module;
}

} // namespace liquid::formats::detail
