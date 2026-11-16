// SPDX-License-Identifier: MIT

#include "build/abi.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

#include "build/repository_access.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto verdict_name(abi_verdict verdict) -> std::string {
    switch (verdict) {
    case abi_verdict::compatible:
        return "compatible";
    case abi_verdict::compatible_additions:
        return "compatible_additions";
    case abi_verdict::incompatible:
        return "incompatible";
    }
    return "incompatible";
}

auto text_attribute(const attestation& value, std::string_view name) -> const std::string* {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    return found == value.attributes.end() ? nullptr : std::get_if<std::string>(&found->value);
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
        co_return std::unexpected(error{errc::unsupported,
                                        "ABI extraction requires a locally built output",
                                        to_string(blob_id)});
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
        co_return std::unexpected(
            error{errc::corrupt, "ABI input length differs from blob record", to_string(blob_id)});
    }
    co_return output;
}

auto descriptor_from_result(repository& source, const action_result& value, digest subject)
    -> runtime::task<std::optional<abi_descriptor>> {
    for (const auto& id : value.abis) {
        auto bytes = co_await repository_detail::build_access::get(source, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto descriptor = objects::decode_abi(*bytes);
        if (!descriptor) {
            co_return std::unexpected(descriptor.error());
        }
        if (same_digest(descriptor->subject, subject)) {
            co_return std::optional<abi_descriptor>{std::move(*descriptor)};
        }
    }
    co_return std::optional<abi_descriptor>{};
}

auto previous_output(repository& source, std::string path)
    -> runtime::task<std::optional<std::pair<action_output, abi_descriptor>>> {
    auto page = co_await source.results({.output = path, .page = {.limit = 1}});
    if (!page) {
        co_return std::unexpected(page.error());
    }
    if (page->records.empty()) {
        co_return std::optional<std::pair<action_output, abi_descriptor>>{};
    }
    const auto& result = page->records.front().value;
    const auto output = std::ranges::find(result.outputs, path, &action_output::path);
    if (output == result.outputs.end()) {
        co_return std::optional<std::pair<action_output, abi_descriptor>>{};
    }
    auto descriptor = co_await descriptor_from_result(source, result, output->blob);
    if (!descriptor) {
        co_return std::unexpected(descriptor.error());
    }
    if (!*descriptor) {
        co_return std::optional<std::pair<action_output, abi_descriptor>>{};
    }
    co_return std::optional{std::pair{*output, std::move(**descriptor)}};
}

auto store_statement(repository& source,
                     const action_result& value,
                     digest current,
                     digest previous,
                     const abi_report& report) -> runtime::task<digest> {
    auto statement =
        attestation{.subject = current,
                    .profile = "abi_compatibility/1",
                    .at = value.finished_at.value_or(value.observed_at),
                    .issuer = value.producer,
                    .subject_kind = attestation_subject_kind::library,
                    .attributes = {{"compatible_with", to_string(previous)},
                                   {"compatibility", verdict_name(report.verdict)},
                                   {"notes", std::to_string(report.changes.size()) + " changes"}}};
    auto signed_statement =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    auto encoded = objects::encode(statement);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::build_access::put_record(source, std::move(*encoded));
}

} // namespace

namespace build_detail {

runtime::task<void> store_abis(repository& source, action_result& value) {
    value.abis.clear();
    value.abi_compatibility.clear();
    for (const auto& output : value.outputs) {
        auto contents = co_await load_blob_contents(source, output.blob);
        if (!contents) {
            if (contents.error().code == errc::not_found ||
                contents.error().code == errc::unsupported) {
                continue;
            }
            co_return std::unexpected(contents.error());
        }
        auto descriptor = co_await runtime::offload(
            [bytes = std::move(*contents)]() mutable { return extract_abi(bytes); });
        if (!descriptor) {
            co_return std::unexpected(descriptor.error());
        }
        if (!*descriptor) {
            if (descriptor->error().code == errc::unsupported) {
                continue;
            }
            co_return std::unexpected(descriptor->error());
        }
        if (!(**descriptor).install_name) {
            continue;
        }
        (**descriptor).subject = output.blob;
        auto previous = co_await previous_output(source, output.path);
        if (!previous) {
            co_return std::unexpected(previous.error());
        }
        auto encoded = objects::encode(**descriptor);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto stored =
            co_await repository_detail::build_access::put_record(source, std::move(*encoded));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        value.abis.push_back(*stored);
        if (!*previous) {
            continue;
        }
        const auto report = diff_abi((*previous)->second, **descriptor);
        auto statement =
            co_await store_statement(source, value, output.blob, (*previous)->first.blob, report);
        if (!statement) {
            co_return std::unexpected(statement.error());
        }
        value.abi_compatibility.push_back(*statement);
    }
    co_return result<void>{};
}

runtime::task<std::vector<wrap_report::abi_break>> abi_breaks(repository& source,
                                                              const action_result& value) {
    auto output = std::vector<wrap_report::abi_break>{};
    for (const auto& id : value.abi_compatibility) {
        auto page = co_await source.attestations({.id = id});
        if (!page) {
            co_return std::unexpected(page.error());
        }
        if (page->records.empty()) {
            co_return std::unexpected(error{
                errc::not_found, "ABI compatibility attestation is not stored", to_string(id)});
        }
        const auto& statement = page->records.front().value;
        const auto* compatibility = text_attribute(statement, "compatibility");
        const auto* compatible_with = text_attribute(statement, "compatible_with");
        if (!compatibility || *compatibility != "incompatible" || !compatible_with) {
            continue;
        }
        auto previous = parse_digest(*compatible_with);
        if (!previous) {
            co_return std::unexpected(previous.error());
        }
        auto old_descriptor = co_await source.abi(*previous);
        auto new_descriptor = co_await source.abi(statement.subject);
        if (!old_descriptor || !new_descriptor) {
            co_return std::unexpected(old_descriptor ? new_descriptor.error()
                                                     : old_descriptor.error());
        }
        const auto current_output = std::ranges::find_if(value.outputs, [&](const auto& candidate) {
            return same_digest(candidate.blob, statement.subject);
        });
        if (current_output == value.outputs.end()) {
            co_return std::unexpected(error{errc::corrupt,
                                            "ABI attestation subject is not a result output",
                                            to_string(statement.subject)});
        }
        output.push_back(
            {current_output->path, *previous, diff_abi(*old_descriptor, *new_descriptor)});
    }
    co_return output;
}

} // namespace build_detail

runtime::task<abi_descriptor> repository::abi(digest library_blob) {
    auto ids = co_await record_ids("abi");
    if (!ids) {
        co_return std::unexpected(ids.error());
    }
    for (const auto& id : *ids) {
        auto bytes = co_await repository_detail::build_access::get(*this, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto descriptor = objects::decode_abi(*bytes);
        if (!descriptor) {
            co_return std::unexpected(descriptor.error());
        }
        if (same_digest(descriptor->subject, library_blob)) {
            co_return std::move(*descriptor);
        }
    }
    co_return std::unexpected(
        error{errc::not_found, "no ABI descriptor for library blob", to_string(library_blob)});
}

} // namespace liquid
