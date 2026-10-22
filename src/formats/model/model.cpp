// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "formats/json.hpp"

namespace liquid::formats::detail {
namespace {

auto text(std::span<const std::byte> value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto object_field(const json::value::object& object, std::string_view name) -> const json::value* {
    const auto found = std::ranges::find(object, name, &std::pair<std::string, json::value>::first);
    return found == object.end() ? nullptr : &found->second;
}

auto string_field(const json::value::object& object, std::string_view name) -> std::string_view {
    const auto* value = object_field(object, name);
    const auto* string = value ? std::get_if<std::string>(&value->data) : nullptr;
    return string ? *string : std::string_view{};
}

auto number_field(const json::value::object& object, std::string_view name) -> std::string_view {
    const auto* value = object_field(object, name);
    const auto* number = value ? std::get_if<json::number>(&value->data) : nullptr;
    return number ? number->text : std::string_view{};
}

class notebook_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> override {
        if (std::filesystem::path{path}.extension() != ".ipynb") {
            return std::optional<format_recognition>{};
        }
        auto parsed = json::parse(text(blob));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return std::optional<format_recognition>{
            format_recognition{format_tier::semantic_verbatim, "cell sources with outputs elided"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto parsed = json::parse(text(blob));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto* root = std::get_if<json::value::object>(&parsed->data);
        const auto* cells_value = root ? object_field(*root, "cells") : nullptr;
        const auto* cells =
            cells_value ? std::get_if<json::value::array>(&cells_value->data) : nullptr;
        if (!cells) {
            return std::unexpected(error{errc::corrupt, "notebook cells are absent", "ipynb"});
        }
        if (cells->size() > limits.maximum_entries) {
            return std::unexpected(
                error{errc::unsupported, "notebook cell limit exceeded", "ipynb"});
        }
        auto output = format_tree{.codec = "jupyter",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        auto remaining = limits.byte_budget;
        for (auto index = std::size_t{}; index < cells->size(); ++index) {
            const auto* cell = std::get_if<json::value::object>(&(*cells)[index].data);
            if (!cell) {
                return std::unexpected(
                    error{errc::corrupt, "notebook cell is not an object", "ipynb"});
            }
            auto source = std::string{};
            if (const auto* field = object_field(*cell, "source")) {
                if (const auto* source_text = std::get_if<std::string>(&field->data)) {
                    source = *source_text;
                } else if (const auto* lines = std::get_if<json::value::array>(&field->data)) {
                    for (const auto& line : *lines) {
                        const auto* value = std::get_if<std::string>(&line.data);
                        if (!value) {
                            return std::unexpected(
                                error{errc::corrupt, "notebook source line is not text", "ipynb"});
                        }
                        source += *value;
                    }
                }
            }
            if (source.size() > remaining) {
                output.truncated = true;
                output.limits.push_back("byte budget reached at cell " + std::to_string(index));
                break;
            }
            remaining -= source.size();
            auto attributes =
                detail::attributes({{"cell_type", std::string{string_field(*cell, "cell_type")}},
                                    {"execution_count", "declared volatile"},
                                    {"outputs", "declared volatile"}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto entry =
                make_entry("cells/" + std::to_string(index),
                           format_entry_kind::member,
                           std::vector<std::byte>{
                               reinterpret_cast<const std::byte*>(source.data()),
                               reinterpret_cast<const std::byte*>(source.data() + source.size())},
                           std::move(*attributes));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return fields_;
    }

private:
    static constexpr auto fields_ = std::array<std::string_view, 2>{"execution_count", "outputs"};
};

class safetensors_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> override {
        if (std::filesystem::path{path}.extension() != ".safetensors" || blob.size() < 8) {
            return std::optional<format_recognition>{};
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "JSON tensor header and byte ranges"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        if (blob.size() < 8) {
            return std::unexpected(
                error{errc::corrupt, "safetensors header is absent", "safetensors"});
        }
        auto length = std::uint64_t{};
        for (auto index = std::size_t{}; index < 8; ++index) {
            length |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(blob[index]))
                      << (index * 8U);
        }
        if (length > blob.size() - 8 || length > limits.byte_budget) {
            return std::unexpected(
                error{errc::corrupt, "invalid safetensors header length", "safetensors"});
        }
        auto parsed = json::parse(text(blob.subspan(8, static_cast<std::size_t>(length))));
        const auto* root = parsed ? std::get_if<json::value::object>(&parsed->data) : nullptr;
        if (!root) {
            return std::unexpected(
                parsed ? error{errc::corrupt, "safetensors header is not an object", "safetensors"}
                       : parsed.error());
        }
        auto output = format_tree{.codec = "safetensors",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        auto remaining = limits.byte_budget;
        for (const auto& [name, value] : *root) {
            if (name == "__metadata__") {
                continue;
            }
            if (output.entries.size() >= limits.maximum_entries) {
                output.truncated = true;
                output.limits.push_back("entry limit reached at tensor " + name);
                break;
            }
            const auto* tensor = std::get_if<json::value::object>(&value.data);
            const auto* offsets_value = tensor ? object_field(*tensor, "data_offsets") : nullptr;
            const auto* offsets =
                offsets_value ? std::get_if<json::value::array>(&offsets_value->data) : nullptr;
            if (!tensor || !offsets || offsets->size() != 2) {
                return std::unexpected(
                    error{errc::corrupt, "invalid safetensors tensor entry", name});
            }
            auto begin_text = std::get_if<json::number>(&(*offsets)[0].data);
            auto end_text = std::get_if<json::number>(&(*offsets)[1].data);
            auto begin = std::uint64_t{};
            auto end = std::uint64_t{};
            if (!begin_text || !end_text ||
                std::from_chars(begin_text->text.data(),
                                begin_text->text.data() + begin_text->text.size(),
                                begin)
                        .ec != std::errc{} ||
                std::from_chars(
                    end_text->text.data(), end_text->text.data() + end_text->text.size(), end)
                        .ec != std::errc{} ||
                begin > end || end > blob.size() - 8 - length) {
                return std::unexpected(
                    error{errc::corrupt, "invalid safetensors tensor range", name});
            }
            // Overlapping ranges still allocate separate copies, so charge every tensor.
            if (end - begin > remaining) {
                output.truncated = true;
                output.limits.push_back("byte budget reached at tensor " + name);
                break;
            }
            remaining -= end - begin;
            const auto* shape = object_field(*tensor, "shape");
            auto shape_text = shape ? json::write(*shape) : result<std::string>{std::string{"[]"}};
            if (!shape_text) {
                return std::unexpected(shape_text.error());
            }
            auto attributes = detail::attributes(
                {{"dtype", std::string{string_field(*tensor, "dtype")}}, {"shape", *shape_text}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            const auto offset = 8 + length + begin;
            auto entry =
                make_entry("tensors/" + name,
                           format_entry_kind::dataset,
                           {blob.begin() + static_cast<std::ptrdiff_t>(offset),
                            blob.begin() + static_cast<std::ptrdiff_t>(offset + end - begin)},
                           std::move(*attributes),
                           format_byte_range{offset, end - begin});
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return {};
    }
};

class pickle_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> override {
        const auto extension = std::filesystem::path{path}.extension();
        if ((extension != ".pkl" && extension != ".pickle") || blob.empty()) {
            return std::optional<format_recognition>{};
        }
        return std::optional<format_recognition>{
            format_recognition{format_tier::semantic_verbatim, "non-executing pickle opcode scan"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto output = format_tree{.codec = "pickle",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        auto strings = std::vector<std::string>{};
        auto globals = std::size_t{};
        auto add_global = [&](std::string name, std::string_view opcode) -> result<void> {
            if (++globals > limits.maximum_entries) {
                return std::unexpected(
                    error{errc::unsupported, "pickle reference limit exceeded", "pickle"});
            }
            auto attributes = detail::attributes(
                {{"opcode", std::string{opcode}}, {"may_deserialise_code", "true"}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto entry = make_entry("globals/" + std::move(name),
                                    format_entry_kind::attribute,
                                    {},
                                    std::move(*attributes));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
            return {};
        };
        auto add_opcode = [&](std::string_view opcode, std::size_t offset) -> result<void> {
            auto attributes = detail::attributes(
                {{"opcode", std::string{opcode}}, {"may_deserialise_code", "true"}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto entry = make_entry("opcodes/" + std::to_string(offset),
                                    format_entry_kind::attribute,
                                    {},
                                    std::move(*attributes));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
            return {};
        };
        for (auto offset = std::size_t{}; offset < blob.size();) {
            const auto opcode = std::to_integer<unsigned char>(blob[offset++]);
            if (opcode == 'c' || opcode == 'i') {
                const auto rest = text(blob.subspan(offset));
                const auto module_end = rest.find('\n');
                const auto name_end = module_end == std::string_view::npos
                                          ? std::string_view::npos
                                          : rest.find('\n', module_end + 1);
                if (module_end == std::string_view::npos || name_end == std::string_view::npos) {
                    return std::unexpected(
                        error{errc::corrupt, "truncated pickle global", "pickle"});
                }
                auto added = add_global(
                    std::string{rest.substr(0, module_end)} + "." +
                        std::string{rest.substr(module_end + 1, name_end - module_end - 1)},
                    opcode == 'c' ? "GLOBAL" : "INST");
                if (!added) {
                    return std::unexpected(added.error());
                }
                offset += name_end + 1;
                continue;
            }
            if (opcode == 0x8c || opcode == 'U') {
                if (offset == blob.size()) {
                    return std::unexpected(
                        error{errc::corrupt, "truncated pickle string", "pickle"});
                }
                const auto length = std::to_integer<unsigned char>(blob[offset++]);
                if (length > blob.size() - offset) {
                    return std::unexpected(
                        error{errc::corrupt, "truncated pickle string", "pickle"});
                }
                strings.emplace_back(text(blob.subspan(offset, length)));
                offset += length;
                continue;
            }
            if (opcode == 'X') {
                if (blob.size() - offset < 4) {
                    return std::unexpected(
                        error{errc::corrupt, "truncated pickle unicode string", "pickle"});
                }
                auto length = std::uint32_t{};
                for (auto index = std::size_t{}; index < 4; ++index) {
                    length |= static_cast<std::uint32_t>(
                                  std::to_integer<unsigned char>(blob[offset + index]))
                              << (index * 8U);
                }
                offset += 4;
                if (length > blob.size() - offset) {
                    return std::unexpected(
                        error{errc::corrupt, "truncated pickle unicode string", "pickle"});
                }
                strings.emplace_back(text(blob.subspan(offset, length)));
                offset += length;
                continue;
            }
            if (opcode == 0x93) {
                if (strings.size() < 2) {
                    return std::unexpected(
                        error{errc::corrupt, "pickle STACK_GLOBAL has no names", "pickle"});
                }
                auto added =
                    add_global(strings[strings.size() - 2] + "." + strings.back(), "STACK_GLOBAL");
                if (!added) {
                    return std::unexpected(added.error());
                }
                strings.resize(strings.size() - 2);
                continue;
            }
            const auto name = std::string_view{opcode == 'R'    ? "REDUCE"
                                               : opcode == 'o'  ? "OBJ"
                                               : opcode == 0x81 ? "NEWOBJ"
                                               : opcode == 'b'  ? "BUILD"
                                                                : ""};
            if (!name.empty()) {
                auto added = add_opcode(name, offset - 1);
                if (!added) {
                    return std::unexpected(added.error());
                }
            }
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return {};
    }
};

template <class Backend>
auto module(std::string_view name,
            const char* const* extensions,
            std::size_t count,
            const char* const* fields = nullptr,
            std::size_t field_count = 0) -> const liquid_format_module_v1* {
    static auto backend = Backend{};
    static auto name_storage = std::string{name};
    static const auto value =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = name_storage.c_str(),
                                               .tier = 2,
                                               .extensions = extensions,
                                               .extension_count = count,
                                               .magic = nullptr,
                                               .magic_count = 0,
                                               .volatile_fields = fields,
                                               .volatile_field_count = field_count,
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

constexpr auto notebook_extensions = std::array<const char*, 1>{".ipynb"};
constexpr auto safetensors_extensions = std::array<const char*, 1>{".safetensors"};
constexpr auto pickle_extensions = std::array<const char*, 2>{".pkl", ".pickle"};
constexpr auto notebook_fields = std::array<const char*, 2>{"execution_count", "outputs"};

} // namespace

auto jupyter_module() -> const liquid_format_module_v1* {
    return module<notebook_backend>("jupyter",
                                    notebook_extensions.data(),
                                    notebook_extensions.size(),
                                    notebook_fields.data(),
                                    notebook_fields.size());
}
auto safetensors_module() -> const liquid_format_module_v1* {
    return module<safetensors_backend>(
        "safetensors", safetensors_extensions.data(), safetensors_extensions.size());
}
auto pickle_module() -> const liquid_format_module_v1* {
    return module<pickle_backend>("pickle", pickle_extensions.data(), pickle_extensions.size());
}

} // namespace liquid::formats::detail
