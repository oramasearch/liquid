// SPDX-License-Identifier: MIT

#include "liquid/delta.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

#include <zstd.h>

#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr std::array<std::byte, 8> delta_magic{std::byte{'L'},
                                               std::byte{'Q'},
                                               std::byte{'D'},
                                               std::byte{'L'},
                                               std::byte{'T'},
                                               std::byte{'A'},
                                               std::byte{1},
                                               std::byte{0}};
constexpr std::size_t encoded_delta_size = delta_magic.size() + 33 + 33 + 1 + 33;
constexpr int zstd_window_log_min = 10;
constexpr int zstd_window_log_max = sizeof(std::size_t) == 4 ? 30 : 31;

auto delta_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto valid_digest_algorithm(std::byte value) -> bool {
    const auto number = std::to_integer<std::uint8_t>(value);
    return number == static_cast<std::uint8_t>(algorithm::blake3) ||
           number == static_cast<std::uint8_t>(algorithm::sha256) ||
           number == static_cast<std::uint8_t>(algorithm::blake3_keyed);
}

void append_digest(std::vector<std::byte>& output, const digest& value) {
    output.push_back(std::byte{static_cast<std::uint8_t>(value.alg)});
    output.insert(output.end(), value.bytes.begin(), value.bytes.end());
}

auto read_digest(std::span<const std::byte> bytes, std::size_t offset) -> result<digest> {
    if (offset + 33 > bytes.size() || !valid_digest_algorithm(bytes[offset])) {
        return std::unexpected(delta_error(errc::corrupt, "invalid delta digest", "delta"));
    }
    auto output = digest{static_cast<algorithm>(std::to_integer<std::uint8_t>(bytes[offset])), {}};
    std::copy_n(bytes.begin() + offset + 1, output.bytes.size(), output.bytes.begin());
    return output;
}

auto zstd_result(size_t value, std::string_view operation) -> result<size_t> {
    if (!ZSTD_isError(value)) {
        return value;
    }
    return std::unexpected(delta_error(errc::corrupt,
                                       ZSTD_getErrorName(value),
                                       std::string{"zstd-patch "} + std::string{operation}));
}

auto set_parameter(ZSTD_CCtx* context, ZSTD_cParameter parameter, int value) -> result<void> {
    auto set = zstd_result(ZSTD_CCtx_setParameter(context, parameter, value), "parameter");
    if (!set) {
        return std::unexpected(set.error());
    }
    return {};
}

auto make_zstd_patch(std::span<const std::byte> source, std::span<const std::byte> target)
    -> result<std::vector<std::byte>> {
    auto context =
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>{ZSTD_createCCtx(), ZSTD_freeCCtx};
    if (!context) {
        return std::unexpected(
            delta_error(errc::internal, "cannot allocate zstd context", "zstd-patch"));
    }
    auto configured = set_parameter(context.get(), ZSTD_c_compressionLevel, 19);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    const auto target_size = std::max<std::size_t>(target.size(), 1);
    const auto wanted_window = static_cast<int>(std::bit_width(target_size - 1));
    const auto window_log = std::clamp(wanted_window, zstd_window_log_min, zstd_window_log_max);
    configured = set_parameter(context.get(), ZSTD_c_windowLog, window_log);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    configured = set_parameter(context.get(), ZSTD_c_enableLongDistanceMatching, 1);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    auto referenced =
        zstd_result(ZSTD_CCtx_refPrefix(context.get(), source.data(), source.size()), "reference");
    if (!referenced) {
        return std::unexpected(referenced.error());
    }
    auto output = std::vector<std::byte>(ZSTD_compressBound(target.size()));
    auto compressed = zstd_result(
        ZSTD_compress2(context.get(), output.data(), output.size(), target.data(), target.size()),
        "create");
    if (!compressed) {
        return std::unexpected(compressed.error());
    }
    output.resize(*compressed);
    return output;
}

auto apply_zstd_patch(std::span<const std::byte> source, std::span<const std::byte> delta)
    -> result<std::vector<std::byte>> {
    const auto size = ZSTD_getFrameContentSize(delta.data(), delta.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN ||
        size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(
            delta_error(errc::corrupt, "delta has no valid target size", "zstd-patch"));
    }
    auto context =
        std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!context) {
        return std::unexpected(
            delta_error(errc::internal, "cannot allocate zstd context", "zstd-patch"));
    }
    auto referenced =
        zstd_result(ZSTD_DCtx_refPrefix(context.get(), source.data(), source.size()), "reference");
    if (!referenced) {
        return std::unexpected(referenced.error());
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    auto decompressed =
        zstd_result(ZSTD_decompressDCtx(
                        context.get(), output.data(), output.size(), delta.data(), delta.size()),
                    "apply");
    if (!decompressed) {
        return std::unexpected(decompressed.error());
    }
    if (*decompressed != output.size()) {
        return std::unexpected(
            delta_error(errc::corrupt, "delta target length mismatch", "zstd-patch"));
    }
    return output;
}

auto attribute_text(const attestation& value, std::string_view name) -> std::optional<std::string> {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    if (found == value.attributes.end()) {
        return std::nullopt;
    }
    const auto* text = std::get_if<std::string>(&found->value);
    return text ? std::optional<std::string>{*text} : std::nullopt;
}

auto delta_algorithm_from_text(std::string_view value) -> std::optional<delta_algorithm> {
    if (value == "zstd-patch") {
        return delta_algorithm::zstd_patch;
    }
    if (value == "executable-sections") {
        return delta_algorithm::executable_sections;
    }
    return std::nullopt;
}

struct delta_candidate {
    delta_record value;
    digest record;
    digest attestation;
    std::vector<std::byte> patch;
};

auto catalog_record(const attestation& statement,
                    const digest& expected_source,
                    const digest& expected_target) -> result<std::optional<digest>> {
    if (statement.profile != "delta/1") {
        return std::optional<digest>{};
    }
    auto verified = verify_attestation(statement);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    if (!verified->verified) {
        return std::unexpected(delta_error(
            errc::corrupt, "invalid delta attestation: " + verified->reason, "delta/1"));
    }
    if (!same_digest(statement.subject, expected_target)) {
        return std::unexpected(
            delta_error(errc::corrupt, "delta attestation has the wrong subject", "delta/1"));
    }
    const auto source = attribute_text(statement, "source");
    const auto target = attribute_text(statement, "target");
    const auto record = attribute_text(statement, "delta");
    if (!source || !target || !record || *source != to_string(expected_source) ||
        *target != to_string(expected_target)) {
        return std::optional<digest>{};
    }
    auto parsed = parse_digest(*record);
    if (!parsed) {
        return std::unexpected(
            delta_error(errc::corrupt, "invalid delta record reference", *record));
    }
    return std::optional<digest>{*parsed};
}

auto validate_candidate(delta_candidate candidate,
                        const attestation& statement,
                        const digest& expected_source,
                        const digest& expected_target) -> result<delta_candidate> {
    const auto algorithm = attribute_text(statement, "algorithm");
    const auto parsed_algorithm = algorithm ? delta_algorithm_from_text(*algorithm) : std::nullopt;
    if (!parsed_algorithm || !same_digest(candidate.value.source, expected_source) ||
        !same_digest(candidate.value.target, expected_target) ||
        candidate.value.algorithm != *parsed_algorithm) {
        return std::unexpected(
            delta_error(errc::corrupt, "delta record does not match its attestation", "delta/1"));
    }
    return candidate;
}

auto find_local_delta(store& source, const digest& base, const digest& target)
    -> runtime::task<std::optional<delta_candidate>> {
    for (const auto& attestation_id : source.attestations(target)) {
        auto statement_bytes = co_await source.get(attestation_id);
        if (!statement_bytes) {
            co_return std::unexpected(statement_bytes.error());
        }
        auto statement = decode_attestation(*statement_bytes);
        if (!statement) {
            co_return std::unexpected(statement.error());
        }
        auto record_id = catalog_record(*statement, base, target);
        if (!record_id) {
            co_return std::unexpected(record_id.error());
        }
        if (!*record_id) {
            continue;
        }
        auto record_bytes = co_await source.get(**record_id);
        if (!record_bytes) {
            co_return std::unexpected(record_bytes.error());
        }
        auto record = decode_delta(*record_bytes);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        auto patch = co_await source.get(record->blob);
        if (!patch) {
            co_return std::unexpected(patch.error());
        }
        auto candidate = validate_candidate(
            delta_candidate{*record, **record_id, attestation_id, std::move(*patch)},
            *statement,
            base,
            target);
        if (!candidate) {
            co_return std::unexpected(candidate.error());
        }
        co_return std::optional<delta_candidate>{std::move(*candidate)};
    }
    co_return std::optional<delta_candidate>{};
}

auto sign_delta_attestation(const delta_record& value, digest record)
    -> runtime::task<attestation> {
    auto signing_key = co_await identity_detail::local_signer();
    if (!signing_key) {
        co_return std::unexpected(signing_key.error());
    }
    auto statement = attestation{
        .subject = value.target,
        .profile = "delta/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .issuer = signing_key->identity(),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"source", to_string(value.source)},
                       {"target", to_string(value.target)},
                       {"algorithm", std::string{to_string(value.algorithm)}},
                       {"delta", to_string(record)}}};
    auto body = objects::encode_attestation_body(statement);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await signing_key->sign("attestation", std::move(*body));
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto signature = identity_detail::canonical_signature(*signed_value);
    if (!signature) {
        co_return std::unexpected(signature.error());
    }
    statement.signature = std::move(*signature);
    co_return statement;
}

} // namespace

std::string_view to_string(delta_algorithm value) noexcept {
    switch (value) {
    case delta_algorithm::zstd_patch:
        return "zstd-patch";
    case delta_algorithm::executable_sections:
        return "executable-sections";
    }
    return "unknown";
}

std::string_view to_string(delta_fetch_method value) noexcept {
    switch (value) {
    case delta_fetch_method::delta:
        return "delta";
    case delta_fetch_method::chunks:
        return "chunks";
    }
    return "unknown";
}

result<std::vector<std::byte>> encode_delta(const delta_record& value) {
    if (to_string(value.algorithm) == "unknown") {
        return std::unexpected(
            delta_error(errc::invalid_argument, "unknown delta algorithm", "delta"));
    }
    auto output = std::vector<std::byte>(delta_magic.begin(), delta_magic.end());
    output.reserve(encoded_delta_size);
    append_digest(output, value.source);
    append_digest(output, value.target);
    output.push_back(std::byte{static_cast<std::uint8_t>(value.algorithm)});
    append_digest(output, value.blob);
    return output;
}

result<delta_record> decode_delta(std::span<const std::byte> bytes) {
    if (bytes.size() != encoded_delta_size ||
        !std::equal(delta_magic.begin(), delta_magic.end(), bytes.begin())) {
        return std::unexpected(delta_error(errc::corrupt, "invalid delta record", "delta"));
    }
    auto source = read_digest(bytes, delta_magic.size());
    auto target = read_digest(bytes, delta_magic.size() + 33);
    auto blob = read_digest(bytes, delta_magic.size() + 67);
    const auto algorithm_value = std::to_integer<std::uint8_t>(bytes[delta_magic.size() + 66]);
    if (!source || !target || !blob ||
        (algorithm_value != static_cast<std::uint8_t>(delta_algorithm::zstd_patch) &&
         algorithm_value != static_cast<std::uint8_t>(delta_algorithm::executable_sections))) {
        return std::unexpected(delta_error(errc::corrupt, "invalid delta record", "delta"));
    }
    return delta_record{*source, *target, static_cast<delta_algorithm>(algorithm_value), *blob};
}

result<std::vector<std::byte>> make_delta(delta_algorithm algorithm,
                                          std::span<const std::byte> source,
                                          std::span<const std::byte> target) {
    if (algorithm == delta_algorithm::zstd_patch) {
        return make_zstd_patch(source, target);
    }
    return std::unexpected(delta_error(
        errc::unsupported, "executable-aware delta encoding is reserved", "executable-sections"));
}

result<std::vector<std::byte>> apply_delta(const delta_record& value,
                                           std::span<const std::byte> source,
                                           std::span<const std::byte> delta) {
    auto source_digest = hash(value.source.alg, source);
    if (!source_digest || !same_digest(*source_digest, value.source)) {
        return std::unexpected(delta_error(
            errc::invalid_argument, "source does not match delta", to_string(value.source)));
    }
    auto delta_digest = hash(value.blob.alg, delta);
    if (!delta_digest || !same_digest(*delta_digest, value.blob)) {
        return std::unexpected(delta_error(
            errc::corrupt, "delta blob does not match record digest", to_string(value.blob)));
    }
    if (value.algorithm != delta_algorithm::zstd_patch) {
        return std::unexpected(delta_error(errc::unsupported,
                                           "executable-aware delta encoding is reserved",
                                           "executable-sections"));
    }
    auto output = apply_zstd_patch(source, delta);
    if (!output) {
        return std::unexpected(output.error());
    }
    auto target_digest = hash(value.target.alg, *output);
    if (!target_digest || !same_digest(*target_digest, value.target)) {
        return std::unexpected(delta_error(
            errc::corrupt, "applied delta does not match target digest", to_string(value.target)));
    }
    return output;
}

runtime::task<delta_creation> create_delta(store& destination,
                                           std::filesystem::path source,
                                           std::filesystem::path target,
                                           delta_algorithm algorithm) {
    auto source_bytes = co_await store_detail::read_file(std::move(source));
    if (!source_bytes) {
        co_return std::unexpected(source_bytes.error());
    }
    auto target_bytes = co_await store_detail::read_file(std::move(target));
    if (!target_bytes) {
        co_return std::unexpected(target_bytes.error());
    }
    auto patch = make_delta(algorithm, *source_bytes, *target_bytes);
    if (!patch) {
        co_return std::unexpected(patch.error());
    }
    auto source_id = co_await destination.put(*source_bytes);
    if (!source_id) {
        co_return std::unexpected(source_id.error());
    }
    auto target_id = co_await destination.put(*target_bytes);
    if (!target_id) {
        co_return std::unexpected(target_id.error());
    }
    const auto patch_size = patch->size();
    auto blob = co_await destination.put(std::move(*patch));
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    auto value = delta_record{*source_id, *target_id, algorithm, *blob};
    auto encoded = encode_delta(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = co_await destination.put(std::move(*encoded));
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto statement = co_await sign_delta_attestation(value, *record);
    if (!statement) {
        co_return std::unexpected(statement.error());
    }
    auto statement_bytes = encode_attestation(*statement);
    if (!statement_bytes) {
        co_return std::unexpected(statement_bytes.error());
    }
    auto statement_id = co_await destination.put(std::move(*statement_bytes));
    if (!statement_id) {
        co_return std::unexpected(statement_id.error());
    }
    co_return delta_creation{
        value, *record, *statement_id, source_bytes->size(), target_bytes->size(), patch_size};
}

runtime::task<void> apply_delta(store& source,
                                digest record,
                                std::filesystem::path base,
                                std::filesystem::path destination) {
    auto record_bytes = co_await source.get(record);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    auto value = decode_delta(*record_bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto source_bytes = co_await store_detail::read_file(std::move(base));
    if (!source_bytes) {
        co_return std::unexpected(source_bytes.error());
    }
    auto patch = co_await source.get(value->blob);
    if (!patch) {
        co_return std::unexpected(patch.error());
    }
    auto output = apply_delta(*value, *source_bytes, *patch);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    co_return co_await store_detail::write_file(std::move(destination), std::move(*output));
}

runtime::task<delta_fetch_result> fetch_delta(store& local,
                                              digest target,
                                              std::filesystem::path source,
                                              std::filesystem::path destination) {
    auto source_bytes = co_await store_detail::read_file(std::move(source));
    if (!source_bytes) {
        co_return std::unexpected(source_bytes.error());
    }
    auto source_id = hash(target.alg, *source_bytes);
    if (!source_id) {
        co_return std::unexpected(source_id.error());
    }
    auto candidate = co_await find_local_delta(local, *source_id, target);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    if (*candidate) {
        auto output = apply_delta((*candidate)->value, *source_bytes, (*candidate)->patch);
        if (!output) {
            co_return std::unexpected(output.error());
        }
        auto written = co_await store_detail::write_file(destination, *output);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        auto cached = co_await local.put(std::move(*output));
        if (!cached) {
            co_return std::unexpected(cached.error());
        }
        co_return delta_fetch_result{delta_fetch_method::delta,
                                     *source_id,
                                     target,
                                     (*candidate)->record,
                                     (*candidate)->attestation};
    }
    auto output = std::vector<std::byte>{};
    auto target_availability = co_await local.has(target);
    if (!target_availability) {
        co_return std::unexpected(target_availability.error());
    }
    if (*target_availability == blob_availability::absent) {
        co_return std::unexpected(
            delta_error(errc::not_found, "target is unavailable", to_string(target)));
    }
    auto bytes = co_await local.get(target);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    output = std::move(*bytes);
    auto actual = hash(target.alg, output);
    if (!actual || !same_digest(*actual, target)) {
        co_return std::unexpected(delta_error(
            errc::corrupt, "fetched chunks do not match target digest", to_string(target)));
    }
    auto written = co_await store_detail::write_file(destination, output);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return delta_fetch_result{
        delta_fetch_method::chunks, *source_id, target, std::nullopt, std::nullopt};
}

} // namespace liquid
