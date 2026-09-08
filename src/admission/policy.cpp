#include "liquid/admission.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/json.hpp"
#include "http1/client.hpp"

namespace liquid {
namespace {

namespace json = formats::json;

auto policy_error(std::string message, std::string context = "admission policy") -> error {
    return {errc::invalid_argument, std::move(message), std::move(context)};
}

auto field(const json::value& value, std::string_view name) -> const json::value* {
    const auto* object = std::get_if<json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto string_field(const json::value& value, std::string_view name) -> result<std::string> {
    const auto* item = field(value, name);
    const auto* text = item ? std::get_if<std::string>(&item->data) : nullptr;
    if (!text) {
        return std::unexpected(policy_error("field must be a string", std::string{name}));
    }
    return *text;
}

auto unsigned_field(const json::value& value, std::string_view name) -> result<std::uint64_t> {
    const auto* item = field(value, name);
    const auto* number = item ? std::get_if<json::number>(&item->data) : nullptr;
    if (!number) {
        return std::unexpected(
            policy_error("field must be an unsigned integer", std::string{name}));
    }
    auto output = std::uint64_t{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        return std::unexpected(
            policy_error("field must be an unsigned integer", std::string{name}));
    }
    return output;
}

auto strings_field(const json::value& value, std::string_view name)
    -> result<std::vector<std::string>> {
    const auto* item = field(value, name);
    const auto* array = item ? std::get_if<json::value::array>(&item->data) : nullptr;
    if (!array) {
        return std::unexpected(policy_error("field must be an array", std::string{name}));
    }
    auto output = std::vector<std::string>{};
    output.reserve(array->size());
    for (const auto& child : *array) {
        const auto* text = std::get_if<std::string>(&child.data);
        if (!text) {
            return std::unexpected(
                policy_error("array entries must be strings", std::string{name}));
        }
        output.push_back(*text);
    }
    return output;
}

auto json_strings(const std::vector<std::string>& values) -> json::value {
    auto output = json::value::array{};
    output.reserve(values.size());
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return json::value{std::move(output)};
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto byte : input) {
        const auto value = std::to_integer<unsigned char>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

auto decode_key(std::string_view input, std::string context, bool empty_allowed)
    -> result<std::vector<std::byte>> {
    if (input.empty() && empty_allowed) {
        return std::vector<std::byte>{};
    }
    if (input.size() != transparency::hash{}.size() * 2) {
        return std::unexpected(policy_error("public key must be 32-byte lowercase hex", context));
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        return -1;
    };
    auto output = std::vector<std::byte>{};
    output.reserve(transparency::hash{}.size());
    for (auto offset = std::size_t{}; offset < input.size(); offset += 2) {
        const auto high = nibble(input[offset]);
        const auto low = nibble(input[offset + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(
                policy_error("public key must be 32-byte lowercase hex", context));
        }
        output.push_back(std::byte{static_cast<unsigned char>((high << 4U) | low)});
    }
    return output;
}

auto witnesses_json(const std::vector<transparency::witness>& witnesses) -> json::value {
    auto output = json::value::array{};
    output.reserve(witnesses.size());
    for (const auto& witness : witnesses) {
        output.emplace_back(json::value::object{
            {"endpoint", json::value{witness.endpoint}},
            {"public_key", json::value{hex(witness.public_key)}},
        });
    }
    return json::value{std::move(output)};
}

auto decode_witnesses(const json::value& value) -> result<std::vector<transparency::witness>> {
    const auto* witnesses = field(value, "witnesses");
    const auto* array = witnesses ? std::get_if<json::value::array>(&witnesses->data) : nullptr;
    if (!array) {
        return std::unexpected(policy_error("field must be an array", "log.trust.witnesses"));
    }
    auto output = std::vector<transparency::witness>{};
    output.reserve(array->size());
    for (const auto& item : *array) {
        auto endpoint = string_field(item, "endpoint");
        auto key_text = string_field(item, "public_key");
        if (!endpoint) {
            return std::unexpected(
                policy_error(endpoint.error().message, "log.trust.witnesses.endpoint"));
        }
        if (!key_text) {
            return std::unexpected(
                policy_error(key_text.error().message, "log.trust.witnesses.public_key"));
        }
        auto key = decode_key(*key_text, "log.trust.witnesses.public_key", false);
        if (!key) {
            return std::unexpected(key.error());
        }
        auto public_key = transparency::hash{};
        std::ranges::copy(*key, public_key.begin());
        output.push_back({.endpoint = std::move(*endpoint), .public_key = public_key});
    }
    return output;
}

} // namespace

std::string_view to_string(admission_mode value) noexcept {
    switch (value) {
    case admission_mode::off:
        return "off";
    case admission_mode::attestation:
        return "attestation";
    case admission_mode::attestation_and_log:
        return "attestation+log";
    }
    return "off";
}

std::string_view to_string(admission_log_mode value) noexcept {
    switch (value) {
    case admission_log_mode::deny:
        return "deny";
    case admission_log_mode::verify:
        return "verify";
    }
    return "deny";
}

std::string_view to_string(log_verdict value) noexcept {
    switch (value) {
    case log_verdict::included:
        return "included";
    case log_verdict::not_included:
        return "not_included";
    case log_verdict::unavailable:
        return "unavailable";
    case log_verdict::denied:
        return "denied";
    }
    return "unavailable";
}

result<std::string> encode_admission_policy(const admission_policy& value) {
    auto log = json::value::object{
        {"mode", json::value{std::string{to_string(value.log.mode)}}},
        {"lake", json::value{value.log.lake}},
        {"trust",
         json::value{json::value::object{
             {"identity",
              json::value{json::value::object{
                  {"taap_id", json::value{value.log.trust.identity.taap_id}},
                  {"public_key", json::value{hex(value.log.trust.identity.public_key)}},
              }}},
             {"witness_threshold",
              json::value{
                  json::number{std::to_string(value.log.trust.configuration.witness_threshold)}}},
             {"witnesses", witnesses_json(value.log.trust.configuration.witnesses)},
         }}},
    };
    if (value.log.max_checkpoint_age_seconds) {
        if (*value.log.max_checkpoint_age_seconds == 0) {
            return std::unexpected(
                policy_error("field must be positive", "log.max_checkpoint_age_seconds"));
        }
        log.emplace_back(
            "max_checkpoint_age_seconds",
            json::value{json::number{std::to_string(*value.log.max_checkpoint_age_seconds)}});
    }
    auto document = json::value{json::value::object{
        {"v", json::value{json::number{std::to_string(value.v)}}},
        {"mode", json::value{std::string{to_string(value.mode)}}},
        {"issuers", json_strings(value.issuers)},
        {"profiles", json_strings(value.profiles)},
        {"exempt", json_strings(value.exempt)},
        {"exempt_namespaces", json_strings(value.exempt_namespaces)},
        {"attestation_source", json::value{value.attestation_source}},
        {"log", json::value{std::move(log)}},
    }};
    return json::write(document);
}

result<admission_policy> decode_admission_policy(std::string_view input) {
    auto document = json::parse(input);
    if (!document) {
        return std::unexpected(document.error());
    }
    if (!std::holds_alternative<json::value::object>(document->data)) {
        return std::unexpected(policy_error("policy must be a JSON object"));
    }
    auto version = unsigned_field(*document, "v");
    auto mode = string_field(*document, "mode");
    auto issuers = strings_field(*document, "issuers");
    auto profiles = strings_field(*document, "profiles");
    auto exempt = strings_field(*document, "exempt");
    auto namespaces = strings_field(*document, "exempt_namespaces");
    auto source = string_field(*document, "attestation_source");
    const auto* log = field(*document, "log");
    if (!version || !mode || !issuers || !profiles || !exempt || !namespaces || !source || !log ||
        !std::holds_alternative<json::value::object>(log->data)) {
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!mode) {
            return std::unexpected(mode.error());
        }
        if (!issuers) {
            return std::unexpected(issuers.error());
        }
        if (!profiles) {
            return std::unexpected(profiles.error());
        }
        if (!exempt) {
            return std::unexpected(exempt.error());
        }
        if (!namespaces) {
            return std::unexpected(namespaces.error());
        }
        if (!source) {
            return std::unexpected(source.error());
        }
        return std::unexpected(policy_error("log field must be an object", "log"));
    }
    auto log_mode = string_field(*log, "mode");
    auto lake = string_field(*log, "lake");
    const auto* trust = field(*log, "trust");
    auto maximum_age = std::optional<std::uint64_t>{};
    if (field(*log, "max_checkpoint_age_seconds")) {
        auto decoded_age = unsigned_field(*log, "max_checkpoint_age_seconds");
        if (!decoded_age) {
            return std::unexpected(
                policy_error(decoded_age.error().message, "log.max_checkpoint_age_seconds"));
        }
        if (*decoded_age == 0) {
            return std::unexpected(
                policy_error("field must be positive", "log.max_checkpoint_age_seconds"));
        }
        maximum_age = *decoded_age;
    }
    if (!log_mode) {
        return std::unexpected(log_mode.error());
    }
    if (!lake) {
        return std::unexpected(lake.error());
    }
    if (!trust || !std::holds_alternative<json::value::object>(trust->data)) {
        return std::unexpected(policy_error("trust field must be an object", "log.trust"));
    }
    if (*version != 1) {
        return std::unexpected(policy_error("unsupported policy version", "v"));
    }
    auto mode_value = admission_mode::off;
    if (*mode == "off") {
        mode_value = admission_mode::off;
    } else if (*mode == "attestation") {
        mode_value = admission_mode::attestation;
    } else if (*mode == "attestation+log") {
        mode_value = admission_mode::attestation_and_log;
    } else {
        return std::unexpected(policy_error("unsupported admission mode", "mode"));
    }
    auto log_mode_value = admission_log_mode::deny;
    if (*log_mode == "deny") {
        log_mode_value = admission_log_mode::deny;
    } else if (*log_mode == "verify") {
        log_mode_value = admission_log_mode::verify;
    } else {
        return std::unexpected(policy_error("unsupported log mode", "log.mode"));
    }
    const auto* identity = field(*trust, "identity");
    if (!identity || !std::holds_alternative<json::value::object>(identity->data)) {
        return std::unexpected(
            policy_error("identity field must be an object", "log.trust.identity"));
    }
    auto identity_id = string_field(*identity, "taap_id");
    auto identity_key_text = string_field(*identity, "public_key");
    auto witness_threshold = unsigned_field(*trust, "witness_threshold");
    auto witnesses = decode_witnesses(*trust);
    if (!identity_id) {
        return std::unexpected(identity_id.error());
    }
    if (!identity_key_text) {
        return std::unexpected(identity_key_text.error());
    }
    auto identity_key = decode_key(*identity_key_text,
                                   "log.trust.identity.public_key",
                                   log_mode_value == admission_log_mode::deny);
    if (!identity_key) {
        return std::unexpected(identity_key.error());
    }
    if (!witness_threshold) {
        return std::unexpected(witness_threshold.error());
    }
    if (!witnesses) {
        return std::unexpected(witnesses.error());
    }
    if (mode_value != admission_mode::off && source->empty()) {
        return std::unexpected(
            policy_error("attestation source is required", "attestation_source"));
    }
    if (log_mode_value == admission_log_mode::verify) {
        if (lake->empty()) {
            return std::unexpected(policy_error("lake URL is required in verify mode", "log.lake"));
        }
        auto valid_lake = http1::validate_url(*lake);
        if (!valid_lake) {
            return std::unexpected(policy_error(valid_lake.error().message, "log.lake"));
        }
        if (identity_id->empty() || !identity_id->starts_with("taap:1:ed25519:") ||
            identity_key->size() != transparency::hash{}.size()) {
            return std::unexpected(
                policy_error("an Ed25519 log identity is required", "log.trust.identity"));
        }
        if (*witness_threshold > witnesses->size()) {
            return std::unexpected(policy_error("witness threshold exceeds configured witnesses",
                                                "log.trust.witness_threshold"));
        }
    }
    return admission_policy{
        .v = *version,
        .mode = mode_value,
        .issuers = std::move(*issuers),
        .profiles = std::move(*profiles),
        .exempt = std::move(*exempt),
        .exempt_namespaces = std::move(*namespaces),
        .attestation_source = std::move(*source),
        .log = {.mode = log_mode_value,
                .lake = std::move(*lake),
                .trust = {.identity = {.taap_id = std::move(*identity_id),
                                       .algorithm = "ed25519",
                                       .public_key = std::move(*identity_key)},
                          .configuration = {.witnesses = std::move(*witnesses),
                                            .witness_threshold = *witness_threshold}},
                .max_checkpoint_age_seconds = maximum_age}};
}

} // namespace liquid
