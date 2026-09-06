#include "liquid/admission.hpp"

#include <charconv>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "formats/json.hpp"

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

result<std::string> encode_admission_policy(const admission_policy& value) {
    auto document = json::value{json::value::object{
        {"v", json::value{json::number{std::to_string(value.v)}}},
        {"mode", json::value{std::string{to_string(value.mode)}}},
        {"issuers", json_strings(value.issuers)},
        {"profiles", json_strings(value.profiles)},
        {"exempt", json_strings(value.exempt)},
        {"exempt_namespaces", json_strings(value.exempt_namespaces)},
        {"attestation_source", json::value{value.attestation_source}},
        {"log",
         json::value{json::value::object{
             {"lake", json::value{value.log.lake}},
             {"max_checkpoint_age_seconds",
              json::value{json::number{std::to_string(value.log.max_checkpoint_age_seconds)}}},
         }}},
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
    auto lake = string_field(*log, "lake");
    auto maximum_age = unsigned_field(*log, "max_checkpoint_age_seconds");
    if (!lake) {
        return std::unexpected(lake.error());
    }
    if (!maximum_age) {
        return std::unexpected(maximum_age.error());
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
    if (mode_value != admission_mode::off && source->empty()) {
        return std::unexpected(
            policy_error("attestation source is required", "attestation_source"));
    }
    if (mode_value == admission_mode::attestation_and_log && lake->empty()) {
        return std::unexpected(policy_error("lake URL is required in log mode", "log.lake"));
    }
    return admission_policy{
        .v = *version,
        .mode = mode_value,
        .issuers = std::move(*issuers),
        .profiles = std::move(*profiles),
        .exempt = std::move(*exempt),
        .exempt_namespaces = std::move(*namespaces),
        .attestation_source = std::move(*source),
        .log = {.lake = std::move(*lake), .max_checkpoint_age_seconds = *maximum_age}};
}

} // namespace liquid
