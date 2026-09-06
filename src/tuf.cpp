#include "liquid/tuf.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "formats/json.hpp"
#include "liquid/repository.hpp"
#include "monocypher-ed25519.h"

namespace liquid::tuf {
namespace {

using json = nlohmann::json;

constexpr auto state_key = std::string_view{"config.tuf"};
constexpr auto top_level_roles =
    std::to_array<std::string_view>({"root", "targets", "snapshot", "timestamp"});

struct parsed_metadata {
    json document;
    std::vector<std::byte> encoded;
    std::uint64_t version{};
    std::chrono::sys_seconds expires;
};

struct client_state {
    parsed_metadata root;
    digest root_digest{algorithm::sha256, {}};
    metadata_versions versions;
};

auto failure(errc code, std::string message, std::string context = "TUF metadata") -> error {
    return {code, std::move(message), std::move(context)};
}

auto text(std::span<const std::byte> input) -> std::string {
    return {reinterpret_cast<const char*>(input.data()), input.size()};
}

auto bytes(std::string_view input) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(input.data());
    return {first, first + input.size()};
}

auto equal(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto sha256(std::span<const std::byte> input) -> result<digest> {
    return hash(algorithm::sha256, input);
}

auto valid_integer(const formats::json::number& number) -> bool {
    const auto value = std::string_view{number.text};
    if (value.find_first_of(".eE") != std::string_view::npos) {
        return false;
    }
    if (value.starts_with('-')) {
        auto parsed = std::int64_t{};
        const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return code == std::errc{} && end == value.data() + value.size();
    }
    auto parsed = std::uint64_t{};
    const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return code == std::errc{} && end == value.data() + value.size();
}

auto validate_json_domain(const formats::json::value& value) -> bool {
    return std::visit(
        [](const auto& item) -> bool {
            using item_type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<item_type, formats::json::number>) {
                return valid_integer(item);
            } else if constexpr (std::is_same_v<item_type, formats::json::value::array>) {
                return std::ranges::all_of(item, validate_json_domain);
            } else if constexpr (std::is_same_v<item_type, formats::json::value::object>) {
                return std::ranges::all_of(
                    item, [](const auto& field) { return validate_json_domain(field.second); });
            }
            return true;
        },
        value.data);
}

auto parse_json(std::span<const std::byte> encoded, std::string_view context) -> result<json> {
    const auto input = text(encoded);
    auto strict = formats::json::parse(input);
    if (!strict) {
        return std::unexpected(
            failure(errc::corrupt, strict.error().message, std::string{context}));
    }
    if (!validate_json_domain(*strict)) {
        return std::unexpected(failure(errc::unsupported,
                                       "TUF canonical JSON permits only 64-bit integers",
                                       std::string{context}));
    }
    try {
        auto output = json::parse(input);
        if (!output.is_object()) {
            return std::unexpected(failure(
                errc::corrupt, "metadata envelope must be an object", std::string{context}));
        }
        return output;
    } catch (const json::exception& caught) {
        return std::unexpected(failure(errc::corrupt, caught.what(), std::string{context}));
    }
}

auto positive_version(const json& value, std::string_view context) -> result<std::uint64_t> {
    if (!value.is_number_unsigned()) {
        return std::unexpected(failure(
            errc::corrupt, "metadata version must be a positive integer", std::string{context}));
    }
    const auto version = value.get<std::uint64_t>();
    if (version == 0) {
        return std::unexpected(failure(
            errc::corrupt, "metadata version must be a positive integer", std::string{context}));
    }
    return version;
}

auto parse_two_digits(std::string_view input, std::size_t offset) -> result<unsigned> {
    auto value = unsigned{};
    const auto [end, code] =
        std::from_chars(input.data() + offset, input.data() + offset + 2, value);
    if (code != std::errc{} || end != input.data() + offset + 2) {
        return std::unexpected(failure(errc::corrupt, "expiry is not UTC RFC 3339"));
    }
    return value;
}

auto parse_expiry(std::string_view input) -> result<std::chrono::sys_seconds> {
    if (input.size() != 20 || input[4] != '-' || input[7] != '-' || input[10] != 'T' ||
        input[13] != ':' || input[16] != ':' || input[19] != 'Z') {
        return std::unexpected(failure(errc::corrupt, "expiry is not UTC RFC 3339"));
    }
    auto year_value = int{};
    const auto [year_end, year_code] = std::from_chars(input.data(), input.data() + 4, year_value);
    auto month_value = parse_two_digits(input, 5);
    auto day_value = parse_two_digits(input, 8);
    auto hour = parse_two_digits(input, 11);
    auto minute = parse_two_digits(input, 14);
    auto second = parse_two_digits(input, 17);
    if (year_code != std::errc{} || year_end != input.data() + 4 || !month_value || !day_value ||
        !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::unexpected(failure(errc::corrupt, "expiry is not UTC RFC 3339"));
    }
    const auto date = std::chrono::year{year_value} / std::chrono::month{*month_value} /
                      std::chrono::day{*day_value};
    if (!date.ok()) {
        return std::unexpected(failure(errc::corrupt, "expiry has an invalid UTC date"));
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

auto parse_metadata(std::vector<std::byte> encoded, std::string_view role)
    -> result<parsed_metadata> {
    auto document = parse_json(encoded, std::string{role} + " metadata");
    if (!document) {
        return std::unexpected(document.error());
    }
    if (!document->contains("signed") || !document->at("signed").is_object() ||
        !document->contains("signatures") || !document->at("signatures").is_array()) {
        return std::unexpected(
            failure(errc::corrupt, "metadata has an invalid signed envelope", std::string{role}));
    }
    const auto& body = document->at("signed");
    if (!body.contains("_type") || !body.at("_type").is_string() ||
        body.at("_type").get<std::string>() != role) {
        return std::unexpected(
            failure(errc::corrupt, "metadata has the wrong _type", std::string{role}));
    }
    if (!body.contains("spec_version") || !body.at("spec_version").is_string()) {
        return std::unexpected(
            failure(errc::corrupt, "metadata has no TUF specification version", std::string{role}));
    }
    const auto specification = body.at("spec_version").get<std::string>();
    if (specification != "1.0" && !specification.starts_with("1.0.")) {
        return std::unexpected(
            failure(errc::unsupported, "metadata must use TUF 1.0", std::string{role}));
    }
    if (!body.contains("version")) {
        return std::unexpected(
            failure(errc::corrupt, "metadata has no version", std::string{role}));
    }
    auto version = positive_version(body.at("version"), role);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (!body.contains("expires") || !body.at("expires").is_string()) {
        return std::unexpected(failure(errc::corrupt, "metadata has no expiry", std::string{role}));
    }
    auto expires = parse_expiry(body.at("expires").get<std::string>());
    if (!expires) {
        auto error = expires.error();
        error.context = std::string{role};
        return std::unexpected(std::move(error));
    }
    return parsed_metadata{
        .document = std::move(*document),
        .encoded = std::move(encoded),
        .version = *version,
        .expires = *expires,
    };
}

auto verify_not_expired(const parsed_metadata& metadata,
                        std::chrono::sys_seconds verification_time,
                        std::string_view role) -> result<void> {
    if (metadata.expires <= verification_time) {
        return std::unexpected(
            failure(errc::unauthorized, "metadata is expired", std::string{role}));
    }
    return {};
}

auto decode_hex(std::string_view input, std::size_t expected, std::string_view context)
    -> result<std::vector<std::byte>> {
    if (input.size() != expected * 2) {
        return std::unexpected(
            failure(errc::corrupt, "hexadecimal value has the wrong length", std::string{context}));
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    auto output = std::vector<std::byte>(expected);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto high = nibble(input[index * 2]);
        const auto low = nibble(input[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(failure(
                errc::corrupt, "hexadecimal value contains a non-hex digit", std::string{context}));
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return output;
}

auto key_material(const json& key, std::string_view key_id) -> result<std::vector<std::byte>> {
    if (!key.is_object() || !key.contains("keytype") || !key.at("keytype").is_string() ||
        key.at("keytype") != "ed25519" || !key.contains("scheme") ||
        !key.at("scheme").is_string() || key.at("scheme") != "ed25519" || !key.contains("keyval") ||
        !key.at("keyval").is_object() || !key.at("keyval").contains("public") ||
        !key.at("keyval").at("public").is_string()) {
        return std::unexpected(failure(
            errc::unsupported, "TUF keys must be Ed25519 public keys", std::string{key_id}));
    }
    const auto canonical = key.dump();
    auto calculated = sha256(bytes(canonical));
    if (!calculated) {
        return std::unexpected(calculated.error());
    }
    if (to_string(*calculated).substr(7) != key_id) {
        return std::unexpected(failure(
            errc::corrupt, "key id does not match its canonical key object", std::string{key_id}));
    }
    return decode_hex(key.at("keyval").at("public").get<std::string>(), 32, key_id);
}

auto role_description(const json& root, std::string_view role) -> result<const json*> {
    const auto& body = root.at("signed");
    if (!body.contains("keys") || !body.at("keys").is_object() || !body.contains("roles") ||
        !body.at("roles").is_object() || !body.at("roles").contains(role) ||
        !body.at("roles").at(role).is_object()) {
        return std::unexpected(
            failure(errc::corrupt, "trusted root is missing role metadata", std::string{role}));
    }
    return &body.at("roles").at(role);
}

auto validate_role(const json& root, std::string_view role) -> result<void> {
    auto description = role_description(root, role);
    if (!description) {
        return std::unexpected(description.error());
    }
    if (!(*description)->contains("keyids") || !(*description)->at("keyids").is_array() ||
        !(*description)->contains("threshold") ||
        !(*description)->at("threshold").is_number_unsigned()) {
        return std::unexpected(
            failure(errc::corrupt, "root role definition is malformed", std::string{role}));
    }
    auto distinct = std::set<std::string>{};
    for (const auto& value : (*description)->at("keyids")) {
        if (!value.is_string()) {
            return std::unexpected(
                failure(errc::corrupt, "root role key id is not a string", std::string{role}));
        }
        const auto key_id = value.get<std::string>();
        if (!distinct.insert(key_id).second) {
            return std::unexpected(
                failure(errc::corrupt, "root role key ids are not unique", std::string{role}));
        }
        const auto& keys = root.at("signed").at("keys");
        if (!keys.contains(key_id)) {
            return std::unexpected(
                failure(errc::corrupt, "root role references an unknown key", std::string{role}));
        }
        auto material = key_material(keys.at(key_id), key_id);
        if (!material) {
            return std::unexpected(material.error());
        }
    }
    const auto threshold = (*description)->at("threshold").get<std::uint64_t>();
    if (threshold == 0 || threshold > distinct.size()) {
        return std::unexpected(
            failure(errc::corrupt, "root role threshold is invalid", std::string{role}));
    }
    return {};
}

auto verify_root_shape(const parsed_metadata& metadata) -> result<void> {
    const auto& body = metadata.document.at("signed");
    if (!body.contains("consistent_snapshot") || !body.at("consistent_snapshot").is_boolean() ||
        !body.contains("keys") || !body.at("keys").is_object() || !body.contains("roles") ||
        !body.at("roles").is_object()) {
        return std::unexpected(
            failure(errc::corrupt, "root metadata is missing required fields", "root"));
    }
    for (const auto role : top_level_roles) {
        auto valid = validate_role(metadata.document, role);
        if (!valid) {
            return std::unexpected(valid.error());
        }
    }
    return {};
}

auto verify_signature(const json& key,
                      std::string_view key_id,
                      std::string_view message,
                      std::string_view encoded_signature) -> bool {
    auto public_key = key_material(key, key_id);
    auto signature = decode_hex(encoded_signature, 64, key_id);
    if (!public_key || !signature) {
        return false;
    }
    return crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(signature->data()),
                                reinterpret_cast<const std::uint8_t*>(public_key->data()),
                                reinterpret_cast<const std::uint8_t*>(message.data()),
                                message.size()) == 0;
}

auto verify_threshold(const parsed_metadata& metadata,
                      const parsed_metadata& trusted_root,
                      std::string_view role) -> result<void> {
    auto description = role_description(trusted_root.document, role);
    if (!description) {
        return std::unexpected(description.error());
    }
    const auto threshold = (*description)->at("threshold").get<std::uint64_t>();
    auto allowed = std::set<std::string>{};
    for (const auto& key_id : (*description)->at("keyids")) {
        allowed.insert(key_id.get<std::string>());
    }
    const auto& keys = trusted_root.document.at("signed").at("keys");
    const auto canonical = metadata.document.at("signed").dump();
    auto counted = std::set<std::string>{};
    for (const auto& signature : metadata.document.at("signatures")) {
        if (!signature.is_object() || !signature.contains("keyid") ||
            !signature.at("keyid").is_string() || !signature.contains("sig") ||
            !signature.at("sig").is_string()) {
            continue;
        }
        const auto key_id = signature.at("keyid").get<std::string>();
        if (!allowed.contains(key_id) || counted.contains(key_id) || !keys.contains(key_id)) {
            continue;
        }
        if (verify_signature(
                keys.at(key_id), key_id, canonical, signature.at("sig").get<std::string>())) {
            counted.insert(key_id);
        }
    }
    if (counted.size() < threshold) {
        return std::unexpected(failure(errc::unauthorized,
                                       "metadata does not meet its unique-key signature threshold",
                                       std::string{role}));
    }
    return {};
}

auto validate_metadata_shape(const parsed_metadata& metadata, std::string_view role)
    -> result<void> {
    const auto& body = metadata.document.at("signed");
    if (role == "targets" && (!body.contains("targets") || !body.at("targets").is_object())) {
        return std::unexpected(
            failure(errc::corrupt, "targets metadata has no target descriptions", "targets"));
    }
    if ((role == "snapshot" || role == "timestamp") &&
        (!body.contains("meta") || !body.at("meta").is_object())) {
        return std::unexpected(
            failure(errc::corrupt, "metadata has no child bindings", std::string{role}));
    }
    return {};
}

auto single_sha256(const json& hashes, std::string_view context) -> result<digest> {
    if (!hashes.is_object() || hashes.size() != 1 || !hashes.contains("sha256") ||
        !hashes.at("sha256").is_string()) {
        return std::unexpected(
            failure(errc::unsupported, "hash set must contain only SHA-256", std::string{context}));
    }
    auto parsed = parse_digest("sha256:" + hashes.at("sha256").get<std::string>());
    if (!parsed || parsed->alg != algorithm::sha256) {
        return std::unexpected(
            failure(errc::corrupt, "SHA-256 digest is malformed", std::string{context}));
    }
    return *parsed;
}

auto verify_raw_binding(const parsed_metadata& parent,
                        std::string_view child_name,
                        std::span<const std::byte> child) -> result<std::uint64_t> {
    const auto& meta = parent.document.at("signed").at("meta");
    if (!meta.contains(child_name) || !meta.at(child_name).is_object()) {
        return std::unexpected(failure(
            errc::corrupt, "required child metadata binding is absent", std::string{child_name}));
    }
    const auto& binding = meta.at(child_name);
    if (!binding.contains("version") || !binding.contains("length") ||
        !binding.at("length").is_number_unsigned() || !binding.contains("hashes")) {
        return std::unexpected(
            failure(errc::corrupt, "child metadata binding is malformed", std::string{child_name}));
    }
    auto version = positive_version(binding.at("version"), child_name);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (binding.at("length").get<std::uint64_t>() != child.size()) {
        return std::unexpected(failure(
            errc::corrupt, "child metadata length does not match", std::string{child_name}));
    }
    auto expected = single_sha256(binding.at("hashes"), child_name);
    auto actual = sha256(child);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    if (!actual) {
        return std::unexpected(actual.error());
    }
    if (!equal(*expected, *actual)) {
        return std::unexpected(
            failure(errc::corrupt, "child metadata hash does not match", std::string{child_name}));
    }
    return *version;
}

auto safe_target_path(std::string_view path) -> bool {
    if (path.empty() || path.starts_with('/') || path.ends_with('/') || path.contains('\\')) {
        return false;
    }
    for (const auto component : std::views::split(path, '/')) {
        const auto value = std::string_view{component.begin(), component.end()};
        if (value.empty() || value == "." || value == "..") {
            return false;
        }
    }
    return true;
}

auto parse_targets(const parsed_metadata& metadata) -> result<std::vector<target_description>> {
    auto output = std::vector<target_description>{};
    for (const auto& [path, value] : metadata.document.at("signed").at("targets").items()) {
        if (!safe_target_path(path) || !value.is_object() || !value.contains("length") ||
            !value.at("length").is_number_unsigned() || !value.contains("hashes")) {
            return std::unexpected(failure(errc::corrupt, "target description is malformed", path));
        }
        auto target_hash = single_sha256(value.at("hashes"), path);
        if (!target_hash) {
            return std::unexpected(target_hash.error());
        }
        output.push_back(target_description{
            .path = path,
            .length = value.at("length").get<std::uint64_t>(),
            .sha256 = *target_hash,
        });
    }
    std::ranges::sort(output, {}, &target_description::path);
    return output;
}

auto encode_state(const client_state& state) -> std::vector<std::byte> {
    const auto document = json{
        {"version", 1},
        {"root",
         {{"version", state.root.version},
          {"sha256", to_string(state.root_digest)},
          {"encoded", text(state.root.encoded)}}},
        {"metadata",
         {{"timestamp", state.versions.timestamp},
          {"snapshot", state.versions.snapshot},
          {"targets", state.versions.targets}}},
    };
    return bytes(document.dump());
}

auto decode_counter(const json& value, std::string_view name) -> result<std::uint64_t> {
    if (!value.contains(name) || !value.at(name).is_number_unsigned()) {
        return std::unexpected(
            failure(errc::corrupt, "trusted state counter is malformed", std::string{name}));
    }
    return value.at(name).get<std::uint64_t>();
}

auto decode_state(std::span<const std::byte> encoded) -> result<client_state> {
    auto document = parse_json(encoded, "repository TUF configuration");
    if (!document) {
        return std::unexpected(document.error());
    }
    if (!document->contains("version") || document->at("version") != 1 ||
        !document->contains("root") || !document->at("root").is_object() ||
        !document->contains("metadata") || !document->at("metadata").is_object()) {
        return std::unexpected(
            failure(errc::corrupt, "trusted state has an unsupported shape", "config.tuf"));
    }
    const auto& root = document->at("root");
    if (!root.contains("encoded") || !root.at("encoded").is_string() || !root.contains("version") ||
        !root.contains("sha256") || !root.at("sha256").is_string()) {
        return std::unexpected(
            failure(errc::corrupt, "trusted root state is malformed", "config.tuf"));
    }
    auto root_version = positive_version(root.at("version"), "config.tuf root");
    auto root_digest = parse_digest(root.at("sha256").get<std::string>());
    auto parsed_root = parse_metadata(bytes(root.at("encoded").get<std::string>()), "root");
    if (!root_version) {
        return std::unexpected(root_version.error());
    }
    if (!root_digest || root_digest->alg != algorithm::sha256) {
        return std::unexpected(
            failure(errc::corrupt, "trusted root digest is malformed", "config.tuf"));
    }
    if (!parsed_root) {
        return std::unexpected(parsed_root.error());
    }
    auto actual_digest = sha256(parsed_root->encoded);
    if (!actual_digest) {
        return std::unexpected(actual_digest.error());
    }
    if (parsed_root->version != *root_version || !equal(*root_digest, *actual_digest)) {
        return std::unexpected(failure(
            errc::corrupt, "trusted root pin does not match its encoded root", "config.tuf"));
    }
    const auto& metadata = document->at("metadata");
    auto timestamp = decode_counter(metadata, "timestamp");
    auto snapshot = decode_counter(metadata, "snapshot");
    auto targets = decode_counter(metadata, "targets");
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    if (!targets) {
        return std::unexpected(targets.error());
    }
    return client_state{
        .root = std::move(*parsed_root),
        .root_digest = *root_digest,
        .versions = {*root_version, *timestamp, *snapshot, *targets},
    };
}

auto load_state(repository& subject) -> runtime::task<client_state> {
    auto stored = co_await subject.metadata(std::string{state_key});
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(
            failure(errc::not_configured, "repository has no pinned TUF root", "config.tuf"));
    }
    co_return decode_state(**stored);
}

auto validate_root(const parsed_metadata& root) -> result<void> {
    auto shape = verify_root_shape(root);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    auto threshold = verify_threshold(root, root, "root");
    if (!threshold) {
        return std::unexpected(threshold.error());
    }
    return {};
}

auto root_record(const client_state& state) -> trusted_root {
    return {
        .version = state.root.version,
        .sha256 = state.root_digest,
        .encoded = state.root.encoded,
    };
}

auto current_time() -> std::chrono::sys_seconds {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

auto require_newer(std::uint64_t candidate, std::uint64_t trusted, std::string_view role)
    -> result<void> {
    if (candidate <= trusted) {
        return std::unexpected(failure(
            errc::conflict, "metadata version would roll back or freeze", std::string{role}));
    }
    return {};
}

auto verify_role(std::vector<std::byte> encoded,
                 std::string_view role,
                 const parsed_metadata& root,
                 std::uint64_t trusted_version,
                 std::chrono::sys_seconds verification_time) -> result<parsed_metadata> {
    auto metadata = parse_metadata(std::move(encoded), role);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    auto shape = validate_metadata_shape(*metadata, role);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    auto expiry = verify_not_expired(*metadata, verification_time, role);
    if (!expiry) {
        return std::unexpected(expiry.error());
    }
    auto version = require_newer(metadata->version, trusted_version, role);
    if (!version) {
        return std::unexpected(version.error());
    }
    auto threshold = verify_threshold(*metadata, root, role);
    if (!threshold) {
        return std::unexpected(threshold.error());
    }
    return metadata;
}

auto advance_roots(client_state state,
                   std::vector<std::vector<std::byte>> roots,
                   std::chrono::sys_seconds verification_time) -> result<client_state> {
    for (auto& encoded_root : roots) {
        auto candidate = parse_metadata(std::move(encoded_root), "root");
        if (!candidate) {
            return std::unexpected(candidate.error());
        }
        if (state.root.version == std::numeric_limits<std::uint64_t>::max() ||
            candidate->version != state.root.version + 1) {
            return std::unexpected(
                failure(errc::conflict, "root metadata must advance exactly one version", "root"));
        }
        auto shape = verify_root_shape(*candidate);
        if (!shape) {
            return std::unexpected(shape.error());
        }
        if (!verify_threshold(*candidate, state.root, "root")) {
            return std::unexpected(failure(
                errc::unauthorized, "new root does not meet the trusted root threshold", "root"));
        }
        if (!verify_threshold(*candidate, *candidate, "root")) {
            return std::unexpected(failure(
                errc::unauthorized, "new root does not meet its own root threshold", "root"));
        }
        auto candidate_hash = sha256(candidate->encoded);
        if (!candidate_hash) {
            return std::unexpected(candidate_hash.error());
        }
        state.root = std::move(*candidate);
        state.root_digest = *candidate_hash;
        state.versions.root = state.root.version;
    }
    auto expiry = verify_not_expired(state.root, verification_time, "root");
    if (!expiry) {
        return std::unexpected(expiry.error());
    }
    return state;
}

struct verified_update {
    client_state state;
    std::vector<target_description> targets;
};

auto verify_update(client_state state,
                   update_metadata metadata,
                   std::chrono::sys_seconds verification_time) -> result<verified_update> {
    auto timestamp = verify_role(std::move(metadata.timestamp),
                                 "timestamp",
                                 state.root,
                                 state.versions.timestamp,
                                 verification_time);
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    auto snapshot_version = verify_raw_binding(*timestamp, "snapshot.json", metadata.snapshot);
    if (!snapshot_version) {
        return std::unexpected(snapshot_version.error());
    }
    auto snapshot = verify_role(std::move(metadata.snapshot),
                                "snapshot",
                                state.root,
                                state.versions.snapshot,
                                verification_time);
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    if (snapshot->version != *snapshot_version) {
        return std::unexpected(
            failure(errc::corrupt, "snapshot version does not match timestamp", "snapshot.json"));
    }
    auto targets_version = verify_raw_binding(*snapshot, "targets.json", metadata.targets);
    if (!targets_version) {
        return std::unexpected(targets_version.error());
    }
    auto targets = verify_role(std::move(metadata.targets),
                               "targets",
                               state.root,
                               state.versions.targets,
                               verification_time);
    if (!targets) {
        return std::unexpected(targets.error());
    }
    if (targets->version != *targets_version) {
        return std::unexpected(
            failure(errc::corrupt, "targets version does not match snapshot", "targets.json"));
    }
    auto descriptions = parse_targets(*targets);
    if (!descriptions) {
        return std::unexpected(descriptions.error());
    }
    state.versions.timestamp = timestamp->version;
    state.versions.snapshot = snapshot->version;
    state.versions.targets = targets->version;
    return verified_update{.state = std::move(state), .targets = std::move(*descriptions)};
}

} // namespace

struct client::impl {
    repository* subject{};
    std::chrono::sys_seconds verification_time;
    client_state state;
    std::vector<target_description> targets;
};

client::client() noexcept = default;
client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;
client::client(std::unique_ptr<impl> implementation) noexcept : impl_(std::move(implementation)) {}

runtime::task<trusted_root> client::pin(repository& subject, std::vector<std::byte> encoded_root) {
    co_return co_await pin(subject, std::move(encoded_root), current_time());
}

runtime::task<trusted_root> client::pin(repository& subject,
                                        std::vector<std::byte> encoded_root,
                                        std::chrono::sys_seconds verification_time) {
    auto root = parse_metadata(std::move(encoded_root), "root");
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto valid = validate_root(*root);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto expiry = verify_not_expired(*root, verification_time, "root");
    if (!expiry) {
        co_return std::unexpected(expiry.error());
    }
    auto root_hash = sha256(root->encoded);
    if (!root_hash) {
        co_return std::unexpected(root_hash.error());
    }
    auto state = client_state{
        .root = std::move(*root),
        .root_digest = *root_hash,
        .versions = {.root = 0, .timestamp = 0, .snapshot = 0, .targets = 0},
    };
    state.versions.root = state.root.version;
    auto stored = co_await subject.set_metadata(std::string{state_key}, encode_state(state));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    co_return root_record(state);
}

runtime::task<trusted_root> client::trusted(repository& subject) {
    auto state = co_await load_state(subject);
    if (!state) {
        co_return std::unexpected(state.error());
    }
    co_return root_record(*state);
}

runtime::task<client> client::open(repository& subject) {
    co_return co_await open(subject, current_time());
}

runtime::task<client> client::open(repository& subject,
                                   std::chrono::sys_seconds verification_time) {
    auto state = co_await load_state(subject);
    if (!state) {
        co_return std::unexpected(state.error());
    }
    auto valid = validate_root(state->root);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    co_return client{std::make_unique<impl>(impl{
        .subject = &subject,
        .verification_time = verification_time,
        .state = std::move(*state),
        .targets = {},
    })};
}

runtime::task<update_result> client::update(update_metadata metadata) {
    if (!impl_ || impl_->subject == nullptr) {
        co_return std::unexpected(failure(errc::invalid_argument, "TUF client is empty"));
    }
    const auto previous_root_version = impl_->state.root.version;
    auto advanced =
        advance_roots(impl_->state, std::move(metadata.roots), impl_->verification_time);
    if (!advanced) {
        co_return std::unexpected(advanced.error());
    }
    if (advanced->root.version != previous_root_version) {
        auto stored_root =
            co_await impl_->subject->set_metadata(std::string{state_key}, encode_state(*advanced));
        if (!stored_root) {
            co_return std::unexpected(stored_root.error());
        }
        impl_->state = *advanced;
    }
    auto verified =
        verify_update(std::move(*advanced), std::move(metadata), impl_->verification_time);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    auto stored = co_await impl_->subject->set_metadata(std::string{state_key},
                                                        encode_state(verified->state));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    impl_->state = std::move(verified->state);
    impl_->targets = verified->targets;
    co_return update_result{.versions = impl_->state.versions,
                            .targets = std::move(verified->targets)};
}

result<target_description> client::target(std::string_view path) const {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "TUF client is empty"));
    }
    const auto found = std::ranges::find(impl_->targets, path, &target_description::path);
    if (found == impl_->targets.end()) {
        return std::unexpected(
            failure(errc::not_found, "target is not trusted", std::string{path}));
    }
    return *found;
}

result<void> client::verify_target(std::string_view path,
                                   std::span<const std::byte> contents) const {
    auto description = target(path);
    if (!description) {
        return std::unexpected(description.error());
    }
    if (description->length != contents.size()) {
        return std::unexpected(
            failure(errc::corrupt, "target length does not match", std::string{path}));
    }
    auto actual = sha256(contents);
    if (!actual) {
        return std::unexpected(actual.error());
    }
    if (!equal(description->sha256, *actual)) {
        return std::unexpected(
            failure(errc::corrupt, "target SHA-256 does not match", std::string{path}));
    }
    return {};
}

} // namespace liquid::tuf
