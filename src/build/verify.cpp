#include "liquid/build.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <ranges>
#include <set>

#include "build/cache.hpp"
#include "build/executor.hpp"
#include "build/repository_access.hpp"
#include "build/verify.hpp"
#include "build/wrap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid::build {
namespace {

auto verification_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

runtime::task<std::vector<std::byte>> read_blob(repository& source, const blob& value) {
    if (value.size > std::numeric_limits<std::size_t>::max()) {
        co_return std::unexpected(
            verification_error(errc::unsupported, "output is too large to explain"));
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(value.size));
    for (const auto& chunk : value.chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != value.size) {
        co_return std::unexpected(
            verification_error(errc::corrupt, "output length differs from its blob record"));
    }
    co_return output;
}

runtime::task<void> add_byte_difference(repository& source,
                                        reproducibility_difference& difference) {
    if (!difference.cached || !difference.reexecuted) {
        co_return liquid::result<void>{};
    }
    auto cached_record = co_await repository_detail::build_access::get(source, *difference.cached);
    auto replayed_record =
        co_await repository_detail::build_access::get(source, *difference.reexecuted);
    if (!cached_record || !replayed_record) {
        co_return std::unexpected(!cached_record ? cached_record.error() : replayed_record.error());
    }
    auto cached_blob = objects::decode_blob(*cached_record);
    auto replayed_blob = objects::decode_blob(*replayed_record);
    if (!cached_blob || !replayed_blob) {
        co_return std::unexpected(!cached_blob ? cached_blob.error() : replayed_blob.error());
    }
    auto cached_bytes = co_await read_blob(source, *cached_blob);
    auto replayed_bytes = co_await read_blob(source, *replayed_blob);
    if (!cached_bytes || !replayed_bytes) {
        co_return std::unexpected(!cached_bytes ? cached_bytes.error() : replayed_bytes.error());
    }
    const auto mismatch = std::ranges::mismatch(*cached_bytes, *replayed_bytes);
    const auto cached_end = mismatch.in1 == cached_bytes->end();
    const auto replayed_end = mismatch.in2 == replayed_bytes->end();
    if (!cached_end || !replayed_end) {
        difference.byte_offset =
            static_cast<std::uint64_t>(std::distance(cached_bytes->begin(), mismatch.in1));
        if (!cached_end) {
            difference.cached_byte = std::to_integer<std::uint8_t>(*mismatch.in1);
        }
        if (!replayed_end) {
            difference.reexecuted_byte = std::to_integer<std::uint8_t>(*mismatch.in2);
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> add_digest_difference(repository& source,
                                          std::vector<reproducibility_difference>& differences,
                                          std::string output,
                                          std::optional<digest> cached,
                                          std::optional<digest> replayed,
                                          bool explain) {
    if (cached && replayed && same_digest(*cached, *replayed)) {
        co_return liquid::result<void>{};
    }
    auto difference = reproducibility_difference{
        .output = std::move(output), .cached = cached, .reexecuted = replayed};
    if (explain) {
        auto explained = co_await add_byte_difference(source, difference);
        if (!explained) {
            co_return std::unexpected(explained.error());
        }
    }
    differences.push_back(std::move(difference));
    co_return liquid::result<void>{};
}

runtime::task<std::vector<reproducibility_difference>> compare(repository& source,
                                                               const action_result& cached,
                                                               const action_result& replayed,
                                                               bool explain) {
    auto differences = std::vector<reproducibility_difference>{};
    if (cached.exit != replayed.exit) {
        differences.push_back({.output = "exit-status"});
    }
    auto cached_outputs = std::map<std::string, digest>{};
    auto replayed_outputs = std::map<std::string, digest>{};
    auto stdout_difference = co_await add_digest_difference(
        source, differences, "stdout", cached.stdout_output, replayed.stdout_output, explain);
    if (!stdout_difference) {
        co_return std::unexpected(stdout_difference.error());
    }
    auto stderr_difference = co_await add_digest_difference(
        source, differences, "stderr", cached.stderr_output, replayed.stderr_output, explain);
    if (!stderr_difference) {
        co_return std::unexpected(stderr_difference.error());
    }
    for (const auto& output : cached.outputs) {
        cached_outputs.emplace(output.path, output.blob);
    }
    for (const auto& output : replayed.outputs) {
        replayed_outputs.emplace(output.path, output.blob);
    }
    auto names = std::set<std::string>{};
    for (const auto& [name, value] : cached_outputs) {
        static_cast<void>(value);
        names.insert(name);
    }
    for (const auto& [name, value] : replayed_outputs) {
        static_cast<void>(value);
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto cached_value = cached_outputs.find(name);
        const auto replayed_value = replayed_outputs.find(name);
        auto difference = co_await add_digest_difference(
            source,
            differences,
            name,
            cached_value == cached_outputs.end() ? std::nullopt
                                                 : std::optional{cached_value->second},
            replayed_value == replayed_outputs.end() ? std::nullopt
                                                     : std::optional{replayed_value->second},
            explain);
        if (!difference) {
            co_return std::unexpected(difference.error());
        }
    }
    co_return differences;
}

auto selected(const objects::action_index_entry& entry,
              const action_result& value,
              const verify_options& options,
              std::uint32_t rate) -> bool {
    switch (options.selection) {
    case verify_selection::sample:
        return !entry.verification && !entry.divergent && (entry.sequence - 1) % rate == 0;
    case verify_selection::all:
        return true;
    case verify_selection::changed:
        return !entry.verification && !entry.divergent;
    case verify_selection::action:
        return options.action && same_digest(value.action, *options.action);
    }
    return false;
}

runtime::task<verified_action> verify_one(repository& source,
                                          build_detail::disk_cache& cache,
                                          const objects::action_index_entry& entry,
                                          const action_result& cached,
                                          bool explain) {
    auto action_bytes = co_await repository_detail::build_access::get(source, cached.action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto sandbox_bytes = co_await repository_detail::build_access::get(source, action->sandbox);
    if (!sandbox_bytes) {
        co_return std::unexpected(sandbox_bytes.error());
    }
    auto expected_wrap_sandbox = objects::encode(wrap_sandbox_definition());
    if (!expected_wrap_sandbox) {
        co_return std::unexpected(expected_wrap_sandbox.error());
    }
    auto replayed = liquid::result<action_result>{};
    if (*sandbox_bytes == *expected_wrap_sandbox) {
        replayed = co_await run_wrapped_action(source, *action, cached, cached.observed_at);
    } else {
        replayed = co_await run_action(
            source, action->input, cached.action, action->sandbox, cached.observed_at);
    }
    if (!replayed) {
        co_return std::unexpected(replayed.error());
    }
    auto differences = co_await compare(source, cached, *replayed, explain);
    if (!differences) {
        co_return std::unexpected(differences.error());
    }
    const auto agreement = differences->empty();
    auto differing_outputs = std::vector<std::string>{};
    for (const auto& difference : *differences) {
        if (difference.output == "exit-status") {
            differing_outputs.push_back("exit-status cached=" + std::to_string(cached.exit) +
                                        " reexecuted=" + std::to_string(replayed->exit));
            continue;
        }
        differing_outputs.push_back(
            difference.output + " cached=" +
            (difference.cached ? to_string(*difference.cached) : "missing") + " reexecuted=" +
            (difference.reexecuted ? to_string(*difference.reexecuted) : "missing"));
    }
    auto signer = repository_detail::build_access::signer(source);
    if (!signer) {
        co_return std::unexpected(signer.error());
    }
    auto attestation = liquid::attestation{
        .subject = entry.result,
        .profile = "reproducibility/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"verified_by", signer->taap_id},
                       {"sandbox", to_string(action->sandbox)},
                       {"agreement", agreement}}};
    if (!differing_outputs.empty()) {
        attestation.attributes.push_back({"differing_outputs", differing_outputs});
    }
    auto signed_attestation =
        co_await repository_detail::build_access::sign_attestation(source, attestation);
    if (!signed_attestation) {
        co_return std::unexpected(signed_attestation.error());
    }
    auto encoded = objects::encode(attestation);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto attestation_id =
        co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!attestation_id) {
        co_return std::unexpected(attestation_id.error());
    }
    auto recorded = co_await cache.record_verification(entry.result, *attestation_id, !agreement);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    if (!agreement) {
        repository_detail::build_access::record_critical_alert(
            source, {cached.action, entry.result, *differences});
    }
    co_return verified_action{
        cached.action, entry.result, *attestation_id, agreement, std::move(*differences)};
}

} // namespace

runtime::task<verify_report> verify(repository& source, verify_options options) {
    if (options.selection == verify_selection::action && !options.action) {
        co_return std::unexpected(
            verification_error(errc::invalid_argument, "verification action is missing"));
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto report = verify_report{};
    auto seen = std::set<std::string>{};
    const auto entries = cache.entries();
    const auto rate = repository_detail::build_access::verification_rate(source);
    for (const auto& entry : entries) {
        if (!seen.insert(to_string(entry.result)).second) {
            continue;
        }
        auto result_bytes = co_await repository_detail::build_access::get(source, entry.result);
        if (!result_bytes) {
            co_return std::unexpected(result_bytes.error());
        }
        auto cached = objects::decode_result(*result_bytes);
        if (!cached) {
            co_return std::unexpected(cached.error());
        }
        if (!selected(entry, *cached, options, rate)) {
            continue;
        }
        ++report.considered;
        auto verified = co_await verify_one(source, cache, entry, *cached, options.explain);
        if (!verified) {
            co_return std::unexpected(verified.error());
        }
        report.actions.push_back(std::move(*verified));
    }
    if (options.selection == verify_selection::action && report.considered == 0) {
        co_return std::unexpected(verification_error(
            errc::not_found, "action is not present in the cache", to_string(*options.action)));
    }
    co_return report;
}

runtime::task<verify_report> verify(std::filesystem::path working_tree, verify_options options) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(verification_error(errc::invalid_argument,
                                                     "verify working tree is not a directory",
                                                     working_tree.string()));
    }
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return co_await verify(*source, std::move(options));
}

} // namespace liquid::build

namespace liquid::build_detail {
namespace {

runtime::task<void> run_scheduled_verification(repository source) {
    auto [yield_error] = co_await runtime::delay(std::chrono::nanoseconds{1});
    if (yield_error) {
        co_return std::unexpected(
            error{errc::internal, yield_error.message(), "scheduled verification"});
    }
    auto verified = co_await build::verify(source, {.selection = verify_selection::sample});
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    co_return result<void>{};
}

} // namespace

void schedule_verification(repository source) {
    runtime::spawn(run_scheduled_verification(std::move(source)), [](result<void>) {});
}

} // namespace liquid::build_detail
