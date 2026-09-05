#include "references.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "liquid/delta.hpp"
#include "objects/records.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> chunk_list_magic{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'C'},
                                                    std::byte{'L'},
                                                    std::byte{'S'},
                                                    std::byte{'T'},
                                                    std::byte{1},
                                                    std::byte{0}};

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    for (unsigned index = 0; index != 2; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 2 > bytes.size())
        return 0;
    return std::uint16_t(std::to_integer<unsigned char>(bytes[at])) |
           (std::uint16_t(std::to_integer<unsigned char>(bytes[at + 1])) << 8U);
}

bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) ||
           value == static_cast<unsigned char>(algorithm::sha256) ||
           value == static_cast<unsigned char>(algorithm::blake3_keyed);
}

std::vector<digest> extract_chunk_list(std::span<const std::byte> bytes) {
    if (bytes.size() < 18 ||
        !std::equal(chunk_list_magic.begin(), chunk_list_magic.end(), bytes.begin())) {
        return {};
    }
    const auto key_size = get_u16(bytes, 16);
    const auto at = 18U + key_size;
    if (at > bytes.size() || (bytes.size() - at) % 33 != 0)
        return {};
    std::vector<digest> output;
    output.reserve((bytes.size() - at) / 33);
    for (std::size_t offset = at; offset < bytes.size(); offset += 33) {
        const auto algorithm_byte = std::to_integer<unsigned char>(bytes[offset]);
        if (!valid_algorithm(algorithm_byte))
            return {};
        digest id{static_cast<algorithm>(algorithm_byte), {}};
        std::copy_n(bytes.begin() + offset + 1, id.bytes.size(), id.bytes.begin());
        output.push_back(id);
    }
    return output;
}

void append_once(std::vector<digest>& output, const digest& value) {
    const auto exists = std::ranges::any_of(output, [&value](const digest& candidate) {
        return candidate.alg == value.alg && candidate.bytes == value.bytes;
    });
    if (!exists)
        output.push_back(value);
}

std::vector<digest> extract_tree(std::span<const std::byte> bytes) {
    const auto tree = objects::decode_tree(bytes);
    if (!tree)
        return {};
    std::vector<digest> output;
    output.reserve(tree->entries.size() * 2);
    for (const auto& entry : tree->entries) {
        append_once(output, entry.id);
        append_once(output, entry.ref);
    }
    return output;
}

std::vector<digest> extract_change(std::span<const std::byte> bytes) {
    const auto change = objects::decode_change(bytes);
    if (!change)
        return {};
    std::vector<digest> output;
    output.reserve(1 + change->parents.size() + change->conflicts.size() +
                   static_cast<std::size_t>(change->symbols.has_value()));
    append_once(output, change->root);
    for (const auto& parent : change->parents)
        append_once(output, parent.id.value);
    for (const auto& conflict : change->conflicts) {
        for (const auto& reference : conflict.refs)
            append_once(output, reference);
    }
    if (change->symbols)
        append_once(output, *change->symbols);
    return output;
}

std::vector<digest> extract_blob(std::span<const std::byte> bytes) {
    const auto blob = objects::decode_blob(bytes);
    if (!blob)
        return {};
    return blob->chunks;
}

std::vector<digest> extract_action(std::span<const std::byte> bytes) {
    const auto action = objects::decode_action(bytes);
    if (!action)
        return {};
    return {action->input.inputs, action->input.toolchain, action->sandbox};
}

std::vector<digest> extract_result(std::span<const std::byte> bytes) {
    const auto result = objects::decode_result(bytes);
    if (!result)
        return {};
    auto output = std::vector<digest>{result->action, result->stdout_output, result->stderr_output};
    if (result->provenance)
        append_once(output, *result->provenance);
    for (const auto& sbom : result->sboms)
        append_once(output, sbom);
    for (const auto& abi : result->abis)
        append_once(output, abi);
    for (const auto& statement : result->abi_compatibility)
        append_once(output, statement);
    for (const auto& action_output : result->outputs)
        append_once(output, action_output.blob);
    return output;
}

std::vector<digest> extract_attestation(std::span<const std::byte> bytes) {
    const auto value = objects::decode_attestation(bytes);
    if (!value)
        return {};
    const auto field = value->profile == "sbom/1"    ? std::string_view{"document"}
                       : value->profile == "delta/1" ? std::string_view{"delta"}
                       : value->profile == "abi_compatibility/1"
                           ? std::string_view{"compatible_with"}
                           : std::string_view{};
    if (field.empty())
        return {};
    const auto reference =
        std::ranges::find(value->attributes, field, &attestation_attribute::name);
    if (reference == value->attributes.end())
        return {};
    const auto* text = std::get_if<std::string>(&reference->value);
    if (!text)
        return {};
    auto id = parse_digest(*text);
    return id ? std::vector<digest>{*id} : std::vector<digest>{};
}

std::vector<digest> extract_abi(std::span<const std::byte> bytes) {
    const auto value = objects::decode_abi(bytes);
    return value ? std::vector<digest>{value->subject} : std::vector<digest>{};
}

std::vector<digest> extract_delta(std::span<const std::byte> bytes) {
    const auto value = decode_delta(bytes);
    if (!value)
        return {};
    return {value->source, value->target, value->blob};
}

std::vector<digest> extract_action_index_root(std::span<const std::byte> bytes) {
    const auto root = objects::decode_action_index_root(bytes);
    if (!root)
        return {};
    auto output = std::vector<digest>{};
    output.reserve(root->entries.size());
    for (const auto& entry : root->entries) {
        if (entry.verification) {
            append_once(output, *entry.verification);
        }
    }
    for (const auto& trace : root->traces) {
        append_once(output, trace);
    }
    return output;
}

std::vector<digest> extract_build_trace(std::span<const std::byte> bytes) {
    const auto trace = objects::decode_build_trace(bytes);
    if (!trace) {
        return {};
    }
    auto output = std::vector<digest>{};
    output.reserve(trace->actions.size());
    for (const auto& action : trace->actions) {
        append_once(output, action.result.value);
    }
    return output;
}

} // namespace

reference_registry::reference_registry() {
    register_kind("chunk-list", extract_chunk_list);
    register_kind("blob", extract_blob);
    register_kind("tree", extract_tree);
    register_kind("change", extract_change);
    register_kind("action", extract_action);
    register_kind("result", extract_result);
    register_kind("action-index-root", extract_action_index_root);
    register_kind("build_trace", extract_build_trace);
    register_kind("attestation", extract_attestation);
    register_kind("delta", extract_delta);
    register_kind("sbom", [](std::span<const std::byte>) { return std::vector<digest>{}; });
    register_kind("abi", extract_abi);
}

void reference_registry::register_kind(std::string kind, reference_extractor extractor) {
    for (std::size_t index = 0; index != extractors_.size(); ++index) {
        if (extractors_[index].first == kind) {
            extractors_[index].second = std::move(extractor);
            return;
        }
    }
    extractors_.emplace_back(std::move(kind), std::move(extractor));
}

extracted_references reference_registry::extract(std::string_view kind,
                                                 std::span<const std::byte> bytes) const {
    for (std::size_t index = 0; index != extractors_.size(); ++index) {
        if (extractors_[index].first != kind)
            continue;
        return {std::string(kind), extractors_[index].second(bytes), true};
    }
    return {std::string(kind), {}, false};
}

std::vector<std::byte>
encode_chunk_list(std::uint64_t size, std::string_view key_id, std::span<const digest> chunks) {
    std::vector<std::byte> descriptor(chunk_list_magic.begin(), chunk_list_magic.end());
    descriptor.reserve(18 + key_id.size() + chunks.size() * 33);
    put_u64(descriptor, size);
    put_u16(descriptor, static_cast<std::uint16_t>(key_id.size()));
    descriptor.insert(descriptor.end(),
                      reinterpret_cast<const std::byte*>(key_id.data()),
                      reinterpret_cast<const std::byte*>(key_id.data() + key_id.size()));
    for (const auto& id : chunks) {
        descriptor.push_back(std::byte{static_cast<unsigned char>(id.alg)});
        descriptor.insert(descriptor.end(), id.bytes.begin(), id.bytes.end());
    }
    return descriptor;
}

std::string detect_object_kind(std::span<const std::byte> bytes) {
    if (bytes.size() >= chunk_list_magic.size() &&
        std::equal(chunk_list_magic.begin(), chunk_list_magic.end(), bytes.begin()))
        return "chunk-list";
    if (objects::decode_tree(bytes))
        return "tree";
    if (objects::decode_blob(bytes))
        return "blob";
    if (objects::decode_change(bytes))
        return "change";
    if (objects::decode_attestation(bytes))
        return "attestation";
    if (decode_delta(bytes))
        return "delta";
    if (objects::decode_action(bytes))
        return "action";
    if (objects::decode_result(bytes))
        return "result";
    if (objects::decode_action_index_root(bytes))
        return "action-index-root";
    if (objects::decode_build_trace(bytes))
        return "build_trace";
    if (objects::decode_sbom(bytes))
        return "sbom";
    if (objects::decode_abi(bytes))
        return "abi";
    return "unknown";
}

} // namespace liquid::store_detail
