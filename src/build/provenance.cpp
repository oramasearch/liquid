// SPDX-License-Identifier: MIT

#include "liquid/build.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string_view>

#include "build/abi.hpp"
#include "build/content.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "build/sbom.hpp"
#include "build/wrap.hpp"
#include "formats/json.hpp"
#include "identity/taap.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace liquid {
namespace {

constexpr auto build_type =
    std::string_view{"https://github.com/oramasearch/liquid/blob/main/docs/library/"
                     "provenance.md#liquid-action-v1"};
constexpr auto predicate_type = std::string_view{"https://slsa.dev/provenance/v1"};

namespace json = formats::json;

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto digest_name(algorithm value) -> std::string {
    switch (value) {
    case algorithm::blake3:
        return "blake3";
    case algorithm::sha256:
        return "sha256";
    case algorithm::blake3_keyed:
        return "blake3k";
    }
    return "unknown";
}

auto digest_hex(const digest& value) -> std::string {
    const auto text = to_string(value);
    const auto separator = text.find(':');
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

auto resource(digest value, std::string kind, std::optional<std::string> name = std::nullopt)
    -> slsa_resource_descriptor {
    return {.uri = "liquid:" + std::move(kind) + ":" + to_string(value),
            .digest = {{digest_name(value.alg), digest_hex(value)}},
            .name = std::move(name)};
}

auto timestamp(std::chrono::sys_seconds value) -> std::string {
    const auto day = std::chrono::floor<std::chrono::days>(value);
    const auto date = std::chrono::year_month_day{day};
    const auto time = std::chrono::hh_mm_ss{value - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()),
                       time.hours().count(),
                       time.minutes().count(),
                       time.seconds().count());
}

auto subject_digest(const action_result& value) -> result<digest> {
    auto subject = value;
    subject.sig.clear();
    subject.provenance.reset();
    auto encoded = objects::encode_result_body(subject);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(value.action.alg, *encoded);
}

auto make_attestation(const objects::action_record& action,
                      const action_result& value,
                      digest subject) -> attestation {
    auto dependencies = std::vector<slsa_resource_descriptor>{};
    dependencies.reserve(value.manifest.size());
    for (const auto& input : value.manifest) {
        dependencies.push_back(resource(input.content, "blob"));
    }
    auto byproducts = std::vector<slsa_resource_descriptor>{};
    byproducts.push_back(resource(value.stdout_output, "blob", "stdout"));
    byproducts.push_back(resource(value.stderr_output, "blob", "stderr"));
    const auto started = value.started_at.value_or(value.observed_at);
    const auto fallback_duration = std::chrono::duration_cast<std::chrono::seconds>(
        value.timing.queued + value.timing.sandbox + value.timing.exec + value.timing.hash +
        value.timing.store);
    const auto finished = value.finished_at.value_or(started + fallback_duration);
    auto predicate = slsa_provenance_v1{
        .build_definition =
            {.build_type = std::string{build_type},
             .external_parameters = {.command = action.input.command,
                                     .cwd = action.input.cwd,
                                     .environment = action.input.env},
             .internal_parameters = {.toolchain = resource(action.input.toolchain, "tree"),
                                     .sandbox = resource(action.sandbox, "sandbox"),
                                     .platform = {{"os", action.input.platform.os},
                                                  {"arch", action.input.platform.arch}}},
             .resolved_dependencies = std::move(dependencies)},
        .run_details = {.builder = {.id = value.producer.taap_id},
                        .metadata = {.invocation_id = to_string(subject),
                                     .started_on = timestamp(started),
                                     .finished_on = timestamp(finished)},
                        .byproducts = std::move(byproducts)}};
    return {.subject = subject,
            .profile = "slsa_provenance/1",
            .at = finished,
            .issuer = value.producer,
            .subject_kind = attestation_subject_kind::artifact,
            .slsa_provenance = std::move(predicate)};
}

auto signed_result_is_valid(const action_result& value) -> result<bool> {
    auto signature = identity_detail::parse_signature(value.sig);
    if (!signature) {
        return false;
    }
    auto body = objects::encode_result_body(value);
    if (!body) {
        return std::unexpected(body.error());
    }
    return verify(value.producer, "result", *body, *signature);
}

auto json_text(std::string value) -> json::value {
    return json::value{std::move(value)};
}

auto json_text_map(const std::map<std::string, std::string>& values) -> json::value {
    auto output = json::value::object{};
    for (const auto& [name, value] : values) {
        output.emplace_back(name, json_text(value));
    }
    return json::value{std::move(output)};
}

auto json_resource(const slsa_resource_descriptor& value) -> json::value {
    auto output =
        json::value::object{{"uri", json_text(value.uri)}, {"digest", json_text_map(value.digest)}};
    if (value.name) {
        output.emplace_back("name", json_text(*value.name));
    }
    return json::value{std::move(output)};
}

auto json_resources(std::span<const slsa_resource_descriptor> values) -> json::value {
    auto output = json::value::array{};
    for (const auto& value : values) {
        output.push_back(json_resource(value));
    }
    return json::value{std::move(output)};
}

auto json_predicate(const slsa_provenance_v1& value) -> json::value {
    auto command = json::value::array{};
    for (const auto& argument : value.build_definition.external_parameters.command) {
        command.push_back(json_text(argument));
    }
    auto external = json::value::object{
        {"command", json::value{std::move(command)}},
        {"cwd", json_text(value.build_definition.external_parameters.cwd)},
        {"environment", json_text_map(value.build_definition.external_parameters.environment)}};
    auto internal = json::value::object{
        {"toolchain", json_resource(value.build_definition.internal_parameters.toolchain)},
        {"sandbox", json_resource(value.build_definition.internal_parameters.sandbox)},
        {"platform", json_text_map(value.build_definition.internal_parameters.platform)}};
    auto definition = json::value::object{
        {"buildType", json_text(value.build_definition.build_type)},
        {"externalParameters", json::value{std::move(external)}},
        {"internalParameters", json::value{std::move(internal)}},
        {"resolvedDependencies", json_resources(value.build_definition.resolved_dependencies)}};
    auto builder = json::value::object{{"id", json_text(value.run_details.builder.id)}};
    auto metadata =
        json::value::object{{"invocationId", json_text(value.run_details.metadata.invocation_id)},
                            {"startedOn", json_text(value.run_details.metadata.started_on)},
                            {"finishedOn", json_text(value.run_details.metadata.finished_on)}};
    auto run = json::value::object{{"builder", json::value{std::move(builder)}},
                                   {"metadata", json::value{std::move(metadata)}},
                                   {"byproducts", json_resources(value.run_details.byproducts)}};
    return json::value{json::value::object{{"buildDefinition", json::value{std::move(definition)}},
                                           {"runDetails", json::value{std::move(run)}}}};
}

auto in_toto_subjects(const provenance_report& report) -> json::value {
    auto output = json::value::array{};
    for (const auto& artifact : report.value.outputs) {
        const auto descriptor = resource(artifact.blob, "blob", artifact.path);
        auto subject = json::value::object{{"name", json_text(artifact.path)},
                                           {"digest", json_text_map(descriptor.digest)}};
        output.emplace_back(std::move(subject));
    }
    if (output.empty()) {
        const auto descriptor = resource(report.statement.subject, "result", "result");
        auto subject = json::value::object{{"name", json_text("result")},
                                           {"digest", json_text_map(descriptor.digest)}};
        output.emplace_back(std::move(subject));
    }
    return json::value{std::move(output)};
}

} // namespace

namespace build_detail {

runtime::task<result_id> store_result_with_provenance(repository& source, action_result& value) {
    auto identity = repository_detail::build_access::signer(source);
    if (!identity) {
        co_return std::unexpected(identity.error());
    }
    value.producer = *identity;
    auto action_bytes = co_await repository_detail::build_access::get(source, value.action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    if (!value.output_tree) {
        auto output_tree = co_await build_detail::store_output_tree(source, value.outputs);
        if (!output_tree) {
            co_return std::unexpected(output_tree.error());
        }
        value.output_tree = *output_tree;
        value.record_version = 4;
    }
    auto sboms = co_await build_detail::store_derived_sboms(source, *action, value);
    if (!sboms) {
        co_return std::unexpected(sboms.error());
    }
    auto abis = co_await build_detail::store_abis(source, value);
    if (!abis) {
        co_return std::unexpected(abis.error());
    }
    auto subject = subject_digest(value);
    if (!subject) {
        co_return std::unexpected(subject.error());
    }
    auto statement = make_attestation(*action, value, *subject);
    auto signed_statement =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    auto statement_bytes = objects::encode(statement);
    if (!statement_bytes) {
        co_return std::unexpected(statement_bytes.error());
    }
    auto statement_id =
        co_await repository_detail::build_access::put_record(source, std::move(*statement_bytes));
    if (!statement_id) {
        co_return std::unexpected(statement_id.error());
    }
    value.provenance = *statement_id;
    auto signed_result = co_await repository_detail::build_access::sign_result(source, value);
    if (!signed_result) {
        co_return std::unexpected(signed_result.error());
    }
    auto result_bytes = objects::encode(value);
    if (!result_bytes) {
        co_return std::unexpected(result_bytes.error());
    }
    auto stored =
        co_await repository_detail::build_access::put_record(source, std::move(*result_bytes));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    co_return result_id{*stored};
}

runtime::task<result_id> import_signed_result(repository& source, const action_result& value) {
    auto signature_valid = signed_result_is_valid(value);
    if (!signature_valid) {
        co_return std::unexpected(signature_valid.error());
    }
    if (!*signature_valid) {
        co_return std::unexpected(
            error{errc::unauthorized, "remote worker result signature is invalid", value.worker});
    }
    if (!value.provenance) {
        co_return std::unexpected(
            error{errc::corrupt, "remote worker result has no provenance", value.worker});
    }
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto stored = co_await repository_detail::build_access::put_record(
        source, std::move(*encoded), value.action.alg);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto provenance = co_await build::verify_provenance(source, result_id{*stored});
    if (!provenance) {
        co_return std::unexpected(provenance.error());
    }
    if (!provenance->verified) {
        co_return std::unexpected(
            error{errc::unauthorized, "remote worker provenance is invalid", provenance->reason});
    }
    for (const auto& output : value.outputs) {
        for (const auto format : {sbom_format::spdx, sbom_format::cyclonedx}) {
            auto sbom =
                co_await build::generate_sbom(source, result_id{*stored}, output.path, format);
            if (!sbom) {
                co_return std::unexpected(sbom.error());
            }
            if (!sbom->verified) {
                co_return std::unexpected(
                    error{errc::unauthorized, "remote worker SBOM is invalid", sbom->reason});
            }
        }
    }
    co_return result_id{*stored};
}

namespace testing {

runtime::task<result_id> store_result_with_provenance(repository& source,
                                                      objects::action_record action,
                                                      action_result& value) {
    auto encoded = objects::encode(action);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto stored = co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    value.action = *stored;
    co_return co_await build_detail::store_result_with_provenance(source, value);
}

} // namespace testing

} // namespace build_detail

namespace build {

runtime::task<provenance_report> verify_provenance(repository& source, result_id id) {
    auto results = co_await source.results({.id = id});
    if (!results) {
        co_return std::unexpected(results.error());
    }
    if (results->records.empty()) {
        co_return std::unexpected(
            error{errc::not_found, "build result is not indexed", to_string(id.value)});
    }
    const auto& value = results->records.front().value;
    if (!value.provenance) {
        co_return std::unexpected(
            error{errc::not_found, "result has no provenance attestation", to_string(id.value)});
    }
    auto statements = co_await source.attestations({.id = *value.provenance});
    if (!statements) {
        co_return std::unexpected(statements.error());
    }
    if (statements->records.empty()) {
        co_return std::unexpected(error{
            errc::not_found, "provenance attestation is not stored", to_string(*value.provenance)});
    }
    const auto& statement = statements->records.front().value;
    auto report = provenance_report{
        .result = id, .attestation = *value.provenance, .statement = statement, .value = value};
    auto result_signature = signed_result_is_valid(value);
    if (!result_signature) {
        co_return std::unexpected(result_signature.error());
    }
    if (!*result_signature) {
        report.reason = "invalid result signature";
        co_return report;
    }
    auto statement_signature = verify_attestation(statement);
    if (!statement_signature) {
        co_return std::unexpected(statement_signature.error());
    }
    if (!statement_signature->verified) {
        report.reason = statement_signature->reason;
        co_return report;
    }
    if (statement.issuer.taap_id != value.producer.taap_id) {
        report.reason = "provenance issuer does not match result producer";
        co_return report;
    }
    auto subject = subject_digest(value);
    if (!subject) {
        co_return std::unexpected(subject.error());
    }
    if (!same_digest(statement.subject, *subject)) {
        report.reason = "provenance subject does not match result";
        co_return report;
    }
    auto action_bytes = co_await repository_detail::build_access::get(source, value.action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto expected = make_attestation(*action, value, *subject);
    auto expected_body = objects::encode_attestation_body(expected);
    auto actual_body = objects::encode_attestation_body(statement);
    if (!expected_body || !actual_body) {
        co_return std::unexpected(expected_body ? actual_body.error() : expected_body.error());
    }
    if (*expected_body != *actual_body) {
        report.reason = "provenance predicate does not match action and result";
        co_return report;
    }
    report.verified = true;
    co_return report;
}

runtime::task<provenance_report> verify_provenance(repository& source, std::string output_path) {
    auto content = std::optional<digest>{};
    auto entry = co_await repository_detail::vfs_access::entry(source, output_path);
    if (entry && entry->kind == node_kind::blob) {
        content = entry->object;
    } else if (!entry && entry.error().code != errc::not_found) {
        co_return std::unexpected(entry.error());
    }
    auto results =
        co_await source.results({.output = output_path, .content = content, .page = {.limit = 1}});
    if (!results) {
        co_return std::unexpected(results.error());
    }
    if (results->records.empty() || results->records.front().sequence == 0) {
        co_return std::unexpected(
            error{errc::not_found, "no provenance result produces this path", output_path});
    }
    co_return co_await verify_provenance(source, results->records.front().id);
}

runtime::task<provenance_report> verify_wrapped_provenance(std::filesystem::path working_tree,
                                                           std::filesystem::path output_path) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code) {
        co_return std::unexpected(error{errc::io, code.message(), working_tree.string()});
    }
    auto absolute = output_path.is_absolute() ? output_path : working_tree / output_path;
    absolute = std::filesystem::weakly_canonical(absolute, code);
    if (code) {
        co_return std::unexpected(error{errc::io, code.message(), absolute.string()});
    }
    const auto relative = absolute.lexically_relative(working_tree);
    const auto relative_text = relative.generic_string();
    if (relative.empty() || relative.is_absolute() || relative_text == ".." ||
        relative_text.starts_with("../")) {
        co_return std::unexpected(error{errc::invalid_argument,
                                        "provenance path is outside the wrapped project",
                                        absolute.string()});
    }
    auto cache_directory = wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return co_await verify_provenance(*source, relative_text);
}

liquid::result<std::string> provenance_json(const provenance_report& report, bool dsse) {
    if (!report.statement.slsa_provenance) {
        return std::unexpected(
            error{errc::invalid_argument, "attestation has no SLSA provenance predicate", {}});
    }
    const auto& predicate = *report.statement.slsa_provenance;
    if (!dsse) {
        auto document = json::value::object{
            {"result", json_text(to_string(report.result.value))},
            {"attestation", json_text(to_string(report.attestation))},
            {"verified", json::value{report.verified}},
            {"profile", json_text(report.statement.profile)},
            {"subject", json_text(to_string(report.statement.subject))},
            {"subjectKind", json_text(std::string{to_string(report.statement.subject_kind)})},
            {"issuer", json_text(report.statement.issuer.taap_id)},
            {"predicateType", json_text(std::string{predicate_type})},
            {"predicate", json_predicate(predicate)}};
        return json::write(json::value{std::move(document)});
    }
    auto statement = json::value::object{{"_type", json_text("https://in-toto.io/Statement/v1")},
                                         {"subject", in_toto_subjects(report)},
                                         {"predicateType", json_text(std::string{predicate_type})},
                                         {"predicate", json_predicate(predicate)}};
    auto payload = json::write(json::value{std::move(statement)});
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto encoded = objects::encode(report.statement);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto payload_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(payload->data()), payload->size()};
    auto signature = json::value::object{
        {"keyid", json_text(report.statement.issuer.taap_id)},
        {"sig", json_text(identity_detail::base64(report.statement.signature))}};
    auto signatures = json::value::array{};
    signatures.emplace_back(std::move(signature));
    auto envelope =
        json::value::object{{"payload", json_text(identity_detail::base64(payload_bytes))},
                            {"payloadType", json_text("application/vnd.in-toto+json")},
                            {"signatures", json::value{std::move(signatures)}},
                            {"liquidAttestation", json_text(identity_detail::base64(*encoded))},
                            {"liquidAttestationDigest", json_text(to_string(report.attestation))}};
    return json::write(json::value{std::move(envelope)});
}

} // namespace build
} // namespace liquid
