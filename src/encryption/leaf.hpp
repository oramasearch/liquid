#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "formats/dotenv.hpp"
#include "formats/json.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid::encryption_detail {

struct leaf_rule {
    std::string key_id;
    std::string encrypted_regex;
};

class leaf_cipher {
public:
    [[nodiscard]] static runtime::task<formats::json::value>
    encrypt_json(formats::json::value document, leaf_rule rule, std::shared_ptr<keyring> keys);
    [[nodiscard]] static runtime::task<formats::json::value>
    decrypt_json(formats::json::value document, leaf_rule rule, std::shared_ptr<keyring> keys);
    [[nodiscard]] static runtime::task<formats::dotenv::document> encrypt_dotenv(
        formats::dotenv::document document, leaf_rule rule, std::shared_ptr<keyring> keys);
    [[nodiscard]] static runtime::task<formats::dotenv::document> decrypt_dotenv(
        formats::dotenv::document document, leaf_rule rule, std::shared_ptr<keyring> keys);
    [[nodiscard]] static runtime::task<std::vector<std::byte>>
    encrypt_file(std::vector<std::byte> plaintext,
                 std::string path,
                 std::string key_id,
                 std::shared_ptr<keyring> keys);
    [[nodiscard]] static runtime::task<std::vector<std::byte>>
    decrypt_file(std::vector<std::byte> ciphertext,
                 std::string path,
                 std::string key_id,
                 std::shared_ptr<keyring> keys);

private:
    struct resolved_key {
        std::string reference;
        std::array<std::byte, 32> material{};
    };

    [[nodiscard]] static runtime::task<resolved_key>
    configuration_key(std::shared_ptr<keyring> keys, std::string key_id, bool create);
};

[[nodiscard]] auto validate_json(const formats::json::value& document, const leaf_rule& rule)
    -> result<void>;
[[nodiscard]] auto validate_dotenv(const formats::dotenv::document& document, const leaf_rule& rule)
    -> result<void>;
[[nodiscard]] auto diff_json(const formats::json::value& left, const formats::json::value& right)
    -> std::vector<std::string>;
[[nodiscard]] auto diff_dotenv(const formats::dotenv::document& left,
                               const formats::dotenv::document& right) -> std::vector<std::string>;
[[nodiscard]] auto preserve_json_envelopes(formats::json::value& plaintext,
                                           const formats::json::value& prior_plaintext,
                                           const formats::json::value& prior_ciphertext,
                                           const leaf_rule& rule) -> result<void>;
[[nodiscard]] auto preserve_dotenv_envelopes(formats::dotenv::document& plaintext,
                                             const formats::dotenv::document& prior_plaintext,
                                             const formats::dotenv::document& prior_ciphertext,
                                             const leaf_rule& rule) -> result<void>;
[[nodiscard]] auto is_envelope(std::string_view value, std::string_view key_id) -> bool;

} // namespace liquid::encryption_detail
