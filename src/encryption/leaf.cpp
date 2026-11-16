// SPDX-License-Identifier: MIT

#include "encryption/leaf.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <regex>
#include <span>

#include "crypto/crypto.hpp"
#include "identity/key_version.hpp"

namespace liquid::encryption_detail {
namespace {

constexpr auto envelope_prefix = std::string_view{"ENC[liquid:v1:"};
constexpr auto leaf_key_context = std::string_view{"liquid configuration leaf encryption v1"};

struct parsed_envelope {
    std::string key_id;
    std::vector<std::byte> boxed;
};

auto leaf_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto byte_span(std::string_view input) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(input.data()), input.size()};
}

auto base64_encode(std::span<const std::byte> input) -> std::string {
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto output = std::string{};
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto remaining = input.size() - offset;
        const auto first = std::to_integer<unsigned>(input[offset]);
        const auto second = remaining > 1 ? std::to_integer<unsigned>(input[offset + 1]) : 0U;
        const auto third = remaining > 2 ? std::to_integer<unsigned>(input[offset + 2]) : 0U;
        const auto bits = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(bits >> 18U) & 0x3fU]);
        output.push_back(alphabet[(bits >> 12U) & 0x3fU]);
        output.push_back(remaining > 1 ? alphabet[(bits >> 6U) & 0x3fU] : '=');
        output.push_back(remaining > 2 ? alphabet[bits & 0x3fU] : '=');
    }
    return output;
}

auto base64_value(char character) -> int {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

auto base64_decode(std::string_view input) -> result<std::vector<std::byte>> {
    if (input.empty() || input.size() % 4 != 0) {
        return std::unexpected(leaf_error(errc::corrupt, "invalid envelope base64"));
    }
    auto output = std::vector<std::byte>{};
    output.reserve(input.size() / 4 * 3);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        const auto final = offset + 4 == input.size();
        const auto padding = input[offset + 3] == '=' ? (input[offset + 2] == '=' ? 2U : 1U) : 0U;
        if ((!final && padding != 0) || input[offset] == '=' || input[offset + 1] == '=' ||
            (input[offset + 2] == '=' && input[offset + 3] != '=')) {
            return std::unexpected(leaf_error(errc::corrupt, "invalid envelope base64 padding"));
        }
        auto values = std::array<int, 4>{};
        for (std::size_t index = 0; index < 4; ++index) {
            if (input[offset + index] == '=') {
                values[index] = 0;
            } else {
                values[index] = base64_value(input[offset + index]);
                if (values[index] < 0) {
                    return std::unexpected(
                        leaf_error(errc::corrupt, "invalid envelope base64 character"));
                }
            }
        }
        const auto bits =
            (static_cast<unsigned>(values[0]) << 18U) | (static_cast<unsigned>(values[1]) << 12U) |
            (static_cast<unsigned>(values[2]) << 6U) | static_cast<unsigned>(values[3]);
        output.push_back(std::byte{static_cast<unsigned char>(bits >> 16U)});
        if (padding < 2) {
            output.push_back(std::byte{static_cast<unsigned char>(bits >> 8U)});
        }
        if (padding == 0) {
            output.push_back(std::byte{static_cast<unsigned char>(bits)});
        }
    }
    return output;
}

auto valid_envelope_key(std::string_view key_id) -> bool {
    return !key_id.empty() && key_id.find_first_of(":]") == std::string_view::npos;
}

auto parse_envelope(std::string_view input) -> result<parsed_envelope> {
    if (!input.starts_with(envelope_prefix) || !input.ends_with(']')) {
        return std::unexpected(leaf_error(errc::unauthorized, "plaintext at encrypted key"));
    }
    auto body = input.substr(envelope_prefix.size(), input.size() - envelope_prefix.size() - 1);
    const auto separator = body.find(':');
    if (separator == std::string_view::npos || separator == 0) {
        return std::unexpected(leaf_error(errc::corrupt, "invalid leaf envelope"));
    }
    auto key_id = std::string{body.substr(0, separator)};
    if (!valid_envelope_key(key_id)) {
        return std::unexpected(leaf_error(errc::corrupt, "invalid leaf envelope key id"));
    }
    auto boxed = base64_decode(body.substr(separator + 1));
    if (!boxed) {
        return std::unexpected(boxed.error());
    }
    if (boxed->size() < 40) {
        return std::unexpected(leaf_error(errc::corrupt, "truncated leaf envelope"));
    }
    return parsed_envelope{std::move(key_id), std::move(*boxed)};
}

auto envelope(std::string_view key_id, std::span<const std::byte> boxed) -> result<std::string> {
    if (!valid_envelope_key(key_id)) {
        return std::unexpected(leaf_error(
            errc::invalid_argument, "key id cannot contain ':' or ']'", std::string{key_id}));
    }
    return std::string{envelope_prefix} + std::string{key_id} + ":" + base64_encode(boxed) + "]";
}

auto pointer_component(std::string_view input) -> std::string {
    auto output = std::string{};
    for (const auto character : input) {
        if (character == '~') {
            output += "~0";
        } else if (character == '/') {
            output += "~1";
        } else {
            output.push_back(character);
        }
    }
    return output;
}

auto compile_regex(std::string_view expression) -> result<std::regex> {
    try {
        return std::regex{expression.begin(), expression.end(), std::regex::ECMAScript};
    } catch (const std::regex_error& caught) {
        return std::unexpected(
            leaf_error(errc::invalid_argument, caught.what(), std::string{expression}));
    }
}

auto matches(const std::regex& expression, std::string_view path) -> bool {
    return std::regex_search(path.begin(), path.end(), expression);
}

auto envelope_matches_key(std::string_view reference, std::string_view key_id) -> bool {
    auto parsed_reference = identity_detail::parse_key_reference(reference);
    auto parsed_key = identity_detail::parse_key_reference(key_id);
    return parsed_reference && parsed_key && parsed_reference->key_id == parsed_key->key_id;
}

auto encode_scalar(const formats::json::value& input) -> result<std::vector<std::byte>> {
    auto output = std::string{};
    if (std::holds_alternative<formats::json::null>(input.data)) {
        output = "n";
    } else if (const auto* boolean = std::get_if<bool>(&input.data)) {
        output = *boolean ? "b1" : "b0";
    } else if (const auto* number = std::get_if<formats::json::number>(&input.data)) {
        output = "d" + number->text;
    } else if (const auto* text = std::get_if<std::string>(&input.data)) {
        output = "s" + *text;
    } else {
        return std::unexpected(leaf_error(errc::invalid_argument, "JSON container is not a leaf"));
    }
    return std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(output.data()),
        reinterpret_cast<const std::byte*>(output.data() + output.size())};
}

auto decode_scalar(std::span<const std::byte> input) -> result<formats::json::value> {
    if (input.empty()) {
        return std::unexpected(leaf_error(errc::corrupt, "empty encrypted JSON leaf"));
    }
    const auto tag = std::to_integer<char>(input.front());
    const auto payload =
        std::string{reinterpret_cast<const char*>(input.data() + 1), input.size() - 1};
    switch (tag) {
    case 'n':
        if (payload.empty()) {
            return formats::json::value{formats::json::null{}};
        }
        break;
    case 'b':
        if (payload == "0") {
            return formats::json::value{false};
        }
        if (payload == "1") {
            return formats::json::value{true};
        }
        break;
    case 'd': {
        auto parsed = formats::json::parse(payload);
        if (parsed && std::holds_alternative<formats::json::number>(parsed->data)) {
            return std::move(*parsed);
        }
        break;
    }
    case 's':
        return formats::json::value{payload};
    default:
        break;
    }
    return std::unexpected(leaf_error(errc::corrupt, "invalid encrypted JSON leaf"));
}

auto seal_text(const crypto_detail::key& key,
               std::string_view key_id,
               std::string_view path,
               std::span<const std::byte> plaintext) -> result<std::string> {
    auto boxed = crypto_detail::seal(key, byte_span(path), plaintext);
    if (!boxed) {
        return std::unexpected(boxed.error());
    }
    return envelope(key_id, *boxed);
}

auto open_text(const crypto_detail::key& key,
               std::string_view key_id,
               std::string_view path,
               std::string_view ciphertext) -> result<std::vector<std::byte>> {
    auto parsed = parse_envelope(ciphertext);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->key_id != key_id) {
        return std::unexpected(leaf_error(
            errc::unauthorized, "leaf envelope names a different key", std::string{path}));
    }
    return crypto_detail::open(key, byte_span(path), parsed->boxed);
}

auto encrypt_json_value(formats::json::value& input,
                        std::string path,
                        const std::regex& expression,
                        std::string_view key_id,
                        const crypto_detail::key& key) -> result<void> {
    if (auto* object = std::get_if<formats::json::value::object>(&input.data)) {
        for (auto& [name, child] : *object) {
            auto transformed = encrypt_json_value(
                child, path + "/" + pointer_component(name), expression, key_id, key);
            if (!transformed) {
                return transformed;
            }
        }
        return {};
    }
    if (auto* array = std::get_if<formats::json::value::array>(&input.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto transformed = encrypt_json_value(
                (*array)[index], path + "/" + std::to_string(index), expression, key_id, key);
            if (!transformed) {
                return transformed;
            }
        }
        return {};
    }
    if (!matches(expression, path)) {
        return {};
    }
    if (const auto* text = std::get_if<std::string>(&input.data);
        text && is_envelope(*text, key_id)) {
        return {};
    }
    auto plaintext = encode_scalar(input);
    if (!plaintext) {
        return std::unexpected(plaintext.error());
    }
    auto ciphertext = seal_text(key, key_id, path, *plaintext);
    crypto_detail::wipe(*plaintext);
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    input = formats::json::value{std::move(*ciphertext)};
    return {};
}

struct encrypted_json_leaf {
    formats::json::value* value{};
    std::string path;
    std::string key_reference;
};

auto collect_encrypted_json_leaves(formats::json::value& input,
                                   std::string path,
                                   const std::regex& expression,
                                   std::string_view key_id,
                                   std::vector<encrypted_json_leaf>& output) -> result<void> {
    if (auto* object = std::get_if<formats::json::value::object>(&input.data)) {
        for (auto& [name, child] : *object) {
            auto collected = collect_encrypted_json_leaves(
                child, path + "/" + pointer_component(name), expression, key_id, output);
            if (!collected) {
                return collected;
            }
        }
        return {};
    }
    if (auto* array = std::get_if<formats::json::value::array>(&input.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto collected = collect_encrypted_json_leaves(
                (*array)[index], path + "/" + std::to_string(index), expression, key_id, output);
            if (!collected) {
                return collected;
            }
        }
        return {};
    }
    if (!matches(expression, path)) {
        return {};
    }
    auto* ciphertext = std::get_if<std::string>(&input.data);
    if (!ciphertext) {
        return std::unexpected(
            leaf_error(errc::unauthorized, "plaintext at encrypted key", std::move(path)));
    }
    auto parsed = parse_envelope(*ciphertext);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (!envelope_matches_key(parsed->key_id, key_id)) {
        return std::unexpected(
            leaf_error(errc::unauthorized, "leaf envelope names a different key", std::move(path)));
    }
    output.push_back({&input, std::move(path), std::move(parsed->key_id)});
    return {};
}

auto validate_json_value(const formats::json::value& input,
                         std::string path,
                         const std::regex& expression,
                         std::string_view key_id) -> result<void> {
    if (const auto* object = std::get_if<formats::json::value::object>(&input.data)) {
        for (const auto& [name, child] : *object) {
            auto valid = validate_json_value(
                child, path + "/" + pointer_component(name), expression, key_id);
            if (!valid) {
                return valid;
            }
        }
        return {};
    }
    if (const auto* array = std::get_if<formats::json::value::array>(&input.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto valid = validate_json_value(
                (*array)[index], path + "/" + std::to_string(index), expression, key_id);
            if (!valid) {
                return valid;
            }
        }
        return {};
    }
    if (!matches(expression, path)) {
        return {};
    }
    const auto* ciphertext = std::get_if<std::string>(&input.data);
    if (!ciphertext || !is_envelope(*ciphertext, key_id)) {
        return std::unexpected(
            leaf_error(errc::unauthorized, "plaintext at encrypted key", std::move(path)));
    }
    return {};
}

void flatten_json(const formats::json::value& input,
                  const std::string& path,
                  std::map<std::string, std::string>& output) {
    if (const auto* object = std::get_if<formats::json::value::object>(&input.data)) {
        for (const auto& [name, child] : *object) {
            flatten_json(child, path + "/" + pointer_component(name), output);
        }
        if (object->empty()) {
            output[path] = "{}";
        }
        return;
    }
    if (const auto* array = std::get_if<formats::json::value::array>(&input.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            flatten_json((*array)[index], path + "/" + std::to_string(index), output);
        }
        if (array->empty()) {
            output[path] = "[]";
        }
        return;
    }
    auto scalar = encode_scalar(input);
    if (scalar) {
        output[path] = std::string{reinterpret_cast<const char*>(scalar->data()), scalar->size()};
    }
}

struct prior_json_leaf {
    std::string plaintext;
    std::string ciphertext;
};

void collect_prior_json(const formats::json::value& plaintext,
                        const formats::json::value& ciphertext,
                        const std::string& path,
                        std::map<std::string, prior_json_leaf>& output) {
    const auto* plain_object = std::get_if<formats::json::value::object>(&plaintext.data);
    const auto* cipher_object = std::get_if<formats::json::value::object>(&ciphertext.data);
    if (plain_object && cipher_object) {
        for (const auto& [name, child] : *plain_object) {
            const auto found = std::ranges::find(
                *cipher_object, name, &std::pair<std::string, formats::json::value>::first);
            if (found != cipher_object->end()) {
                collect_prior_json(
                    child, found->second, path + "/" + pointer_component(name), output);
            }
        }
        return;
    }
    const auto* plain_array = std::get_if<formats::json::value::array>(&plaintext.data);
    const auto* cipher_array = std::get_if<formats::json::value::array>(&ciphertext.data);
    if (plain_array && cipher_array) {
        const auto count = std::min(plain_array->size(), cipher_array->size());
        for (std::size_t index = 0; index < count; ++index) {
            collect_prior_json((*plain_array)[index],
                               (*cipher_array)[index],
                               path + "/" + std::to_string(index),
                               output);
        }
        return;
    }
    const auto* envelope_text = std::get_if<std::string>(&ciphertext.data);
    auto encoded = encode_scalar(plaintext);
    if (envelope_text && encoded) {
        output[path] = {
            std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()},
            *envelope_text};
    }
}

void apply_prior_json(formats::json::value& plaintext,
                      const std::string& path,
                      const std::regex& expression,
                      std::string_view key_id,
                      const std::map<std::string, prior_json_leaf>& prior) {
    if (auto* object = std::get_if<formats::json::value::object>(&plaintext.data)) {
        for (auto& [name, child] : *object) {
            apply_prior_json(
                child, path + "/" + pointer_component(name), expression, key_id, prior);
        }
        return;
    }
    if (auto* array = std::get_if<formats::json::value::array>(&plaintext.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            apply_prior_json(
                (*array)[index], path + "/" + std::to_string(index), expression, key_id, prior);
        }
        return;
    }
    if (!matches(expression, path)) {
        return;
    }
    const auto found = prior.find(path);
    auto encoded = encode_scalar(plaintext);
    if (found != prior.end() && encoded) {
        const auto current =
            std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()};
        if (current == found->second.plaintext && is_envelope(found->second.ciphertext, key_id)) {
            plaintext = formats::json::value{found->second.ciphertext};
        }
    }
}

auto changed_keys(const std::map<std::string, std::string>& left,
                  const std::map<std::string, std::string>& right) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    auto left_it = left.begin();
    auto right_it = right.begin();
    while (left_it != left.end() || right_it != right.end()) {
        if (right_it == right.end() ||
            (left_it != left.end() && left_it->first < right_it->first)) {
            output.push_back(left_it->first);
            ++left_it;
        } else if (left_it == left.end() || right_it->first < left_it->first) {
            output.push_back(right_it->first);
            ++right_it;
        } else {
            if (left_it->second != right_it->second) {
                output.push_back(left_it->first);
            }
            ++left_it;
            ++right_it;
        }
    }
    return output;
}

} // namespace

auto leaf_cipher::configuration_key(std::shared_ptr<keyring> keys, std::string key_id, bool create)
    -> runtime::task<resolved_key> {
    if (!keys) {
        co_return std::unexpected(
            leaf_error(errc::unauthorized, "encryption keyring is unavailable", key_id));
    }
    auto reference = std::move(key_id);
    if (create) {
        auto writable = co_await keys->write_key(reference);
        if (!writable) {
            co_return std::unexpected(writable.error());
        }
        reference = std::move(*writable);
    }
    auto data = co_await keys->data_key(reference, create);
    if (!data) {
        co_return std::unexpected(data.error());
    }
    auto derived = crypto_detail::derive_key(*data, leaf_key_context);
    crypto_detail::wipe(*data);
    if (!derived) {
        co_return std::unexpected(derived.error());
    }
    co_return resolved_key{std::move(reference), std::move(*derived)};
}

auto leaf_cipher::encrypt_json(formats::json::value document,
                               leaf_rule rule,
                               std::shared_ptr<keyring> keys)
    -> runtime::task<formats::json::value> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        co_return std::unexpected(expression.error());
    }
    auto key = co_await configuration_key(keys, rule.key_id, true);
    if (!key) {
        co_return std::unexpected(key.error());
    }
    auto transformed = encrypt_json_value(document, "", *expression, key->reference, key->material);
    crypto_detail::wipe(key->material);
    if (!transformed) {
        co_return std::unexpected(transformed.error());
    }
    auto valid = validate_json(document, rule);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    co_return document;
}

auto leaf_cipher::decrypt_json(formats::json::value document,
                               leaf_rule rule,
                               std::shared_ptr<keyring> keys)
    -> runtime::task<formats::json::value> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        co_return std::unexpected(expression.error());
    }
    auto leaves = std::vector<encrypted_json_leaf>{};
    auto collected = collect_encrypted_json_leaves(document, "", *expression, rule.key_id, leaves);
    if (!collected) {
        co_return std::unexpected(collected.error());
    }
    for (auto& leaf : leaves) {
        auto key = co_await configuration_key(keys, leaf.key_reference, false);
        if (!key) {
            co_return std::unexpected(key.error());
        }
        const auto& ciphertext = std::get<std::string>(leaf.value->data);
        auto plaintext = open_text(key->material, key->reference, leaf.path, ciphertext);
        crypto_detail::wipe(key->material);
        if (!plaintext) {
            co_return std::unexpected(plaintext.error());
        }
        auto decoded = decode_scalar(*plaintext);
        crypto_detail::wipe(*plaintext);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        *leaf.value = std::move(*decoded);
    }
    co_return document;
}

auto leaf_cipher::encrypt_dotenv(formats::dotenv::document document,
                                 leaf_rule rule,
                                 std::shared_ptr<keyring> keys)
    -> runtime::task<formats::dotenv::document> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        co_return std::unexpected(expression.error());
    }
    auto key = co_await configuration_key(keys, rule.key_id, true);
    if (!key) {
        co_return std::unexpected(key.error());
    }
    for (auto& line : document.lines) {
        if (line.kind != formats::dotenv::line_kind::assignment) {
            continue;
        }
        const auto path = "/" + pointer_component(line.key);
        if (!matches(*expression, path) || is_envelope(line.value, rule.key_id)) {
            continue;
        }
        auto ciphertext = seal_text(key->material, key->reference, path, byte_span(line.value));
        if (!ciphertext) {
            crypto_detail::wipe(key->material);
            co_return std::unexpected(ciphertext.error());
        }
        line.value = std::move(*ciphertext);
    }
    crypto_detail::wipe(key->material);
    auto valid = validate_dotenv(document, rule);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    co_return document;
}

auto leaf_cipher::decrypt_dotenv(formats::dotenv::document document,
                                 leaf_rule rule,
                                 std::shared_ptr<keyring> keys)
    -> runtime::task<formats::dotenv::document> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        co_return std::unexpected(expression.error());
    }
    for (auto& line : document.lines) {
        if (line.kind != formats::dotenv::line_kind::assignment) {
            continue;
        }
        const auto path = "/" + pointer_component(line.key);
        if (!matches(*expression, path)) {
            continue;
        }
        auto parsed = parse_envelope(line.value);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        if (!envelope_matches_key(parsed->key_id, rule.key_id)) {
            co_return std::unexpected(
                leaf_error(errc::unauthorized, "leaf envelope names a different key", path));
        }
        auto key = co_await configuration_key(keys, parsed->key_id, false);
        if (!key) {
            co_return std::unexpected(key.error());
        }
        auto plaintext = open_text(key->material, key->reference, path, line.value);
        crypto_detail::wipe(key->material);
        if (!plaintext) {
            co_return std::unexpected(plaintext.error());
        }
        line.value =
            std::string{reinterpret_cast<const char*>(plaintext->data()), plaintext->size()};
        crypto_detail::wipe(*plaintext);
    }
    co_return document;
}

auto leaf_cipher::encrypt_file(std::vector<std::byte> plaintext,
                               std::string path,
                               std::string key_id,
                               std::shared_ptr<keyring> keys)
    -> runtime::task<std::vector<std::byte>> {
    auto key = co_await configuration_key(keys, key_id, true);
    if (!key) {
        co_return std::unexpected(key.error());
    }
    auto ciphertext = seal_text(key->material, key->reference, path, plaintext);
    crypto_detail::wipe(key->material);
    crypto_detail::wipe(plaintext);
    if (!ciphertext) {
        co_return std::unexpected(ciphertext.error());
    }
    co_return std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(ciphertext->data()),
        reinterpret_cast<const std::byte*>(ciphertext->data() + ciphertext->size())};
}

auto leaf_cipher::decrypt_file(std::vector<std::byte> ciphertext,
                               std::string path,
                               std::string key_id,
                               std::shared_ptr<keyring> keys)
    -> runtime::task<std::vector<std::byte>> {
    const auto text =
        std::string_view{reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size()};
    auto parsed = parse_envelope(text);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (!envelope_matches_key(parsed->key_id, key_id)) {
        co_return std::unexpected(
            leaf_error(errc::unauthorized, "file envelope names a different key", path));
    }
    auto key = co_await configuration_key(keys, parsed->key_id, false);
    if (!key) {
        co_return std::unexpected(key.error());
    }
    auto plaintext = open_text(key->material, key->reference, path, text);
    crypto_detail::wipe(key->material);
    if (!plaintext) {
        co_return std::unexpected(plaintext.error());
    }
    co_return plaintext;
}

auto validate_json(const formats::json::value& document, const leaf_rule& rule) -> result<void> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        return std::unexpected(expression.error());
    }
    return validate_json_value(document, "", *expression, rule.key_id);
}

auto validate_dotenv(const formats::dotenv::document& document, const leaf_rule& rule)
    -> result<void> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        return std::unexpected(expression.error());
    }
    for (const auto& line : document.lines) {
        if (line.kind != formats::dotenv::line_kind::assignment) {
            continue;
        }
        const auto path = "/" + pointer_component(line.key);
        if (matches(*expression, path) && !is_envelope(line.value, rule.key_id)) {
            return std::unexpected(
                leaf_error(errc::unauthorized, "plaintext at encrypted key", path));
        }
    }
    return {};
}

auto diff_json(const formats::json::value& left, const formats::json::value& right)
    -> std::vector<std::string> {
    auto left_values = std::map<std::string, std::string>{};
    auto right_values = std::map<std::string, std::string>{};
    flatten_json(left, "", left_values);
    flatten_json(right, "", right_values);
    return changed_keys(left_values, right_values);
}

auto diff_dotenv(const formats::dotenv::document& left, const formats::dotenv::document& right)
    -> std::vector<std::string> {
    auto left_values = std::map<std::string, std::string>{};
    auto right_values = std::map<std::string, std::string>{};
    for (const auto& line : left.lines) {
        if (line.kind == formats::dotenv::line_kind::assignment) {
            left_values["/" + pointer_component(line.key)] = line.value;
        }
    }
    for (const auto& line : right.lines) {
        if (line.kind == formats::dotenv::line_kind::assignment) {
            right_values["/" + pointer_component(line.key)] = line.value;
        }
    }
    return changed_keys(left_values, right_values);
}

auto preserve_json_envelopes(formats::json::value& plaintext,
                             const formats::json::value& prior_plaintext,
                             const formats::json::value& prior_ciphertext,
                             const leaf_rule& rule) -> result<void> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        return std::unexpected(expression.error());
    }
    auto prior = std::map<std::string, prior_json_leaf>{};
    collect_prior_json(prior_plaintext, prior_ciphertext, "", prior);
    apply_prior_json(plaintext, "", *expression, rule.key_id, prior);
    return {};
}

auto preserve_dotenv_envelopes(formats::dotenv::document& plaintext,
                               const formats::dotenv::document& prior_plaintext,
                               const formats::dotenv::document& prior_ciphertext,
                               const leaf_rule& rule) -> result<void> {
    auto expression = compile_regex(rule.encrypted_regex);
    if (!expression) {
        return std::unexpected(expression.error());
    }
    auto prior_plain = std::map<std::string, std::string>{};
    auto prior_cipher = std::map<std::string, std::string>{};
    for (const auto& line : prior_plaintext.lines) {
        if (line.kind == formats::dotenv::line_kind::assignment) {
            prior_plain[line.key] = line.value;
        }
    }
    for (const auto& line : prior_ciphertext.lines) {
        if (line.kind == formats::dotenv::line_kind::assignment) {
            prior_cipher[line.key] = line.value;
        }
    }
    for (auto& line : plaintext.lines) {
        const auto path = "/" + pointer_component(line.key);
        if (line.kind != formats::dotenv::line_kind::assignment || !matches(*expression, path)) {
            continue;
        }
        const auto plain = prior_plain.find(line.key);
        const auto cipher = prior_cipher.find(line.key);
        if (plain != prior_plain.end() && cipher != prior_cipher.end() &&
            plain->second == line.value && is_envelope(cipher->second, rule.key_id)) {
            line.value = cipher->second;
        }
    }
    return {};
}

auto is_envelope(std::string_view value, std::string_view key_id) -> bool {
    auto parsed = parse_envelope(value);
    return parsed && envelope_matches_key(parsed->key_id, key_id);
}

} // namespace liquid::encryption_detail
