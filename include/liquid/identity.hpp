#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

namespace store_detail {
class store_backend;
}

class keyring;
class signer;

namespace identity_detail {
[[nodiscard]] LIQUID_EXPORT runtime::task<signer> local_signer();
}

struct LIQUID_EXPORT actor {
    std::string taap_id;
    std::string algorithm;
    std::string fingerprint;
    std::vector<std::byte> public_key;
    std::string key_fingerprint;

    [[nodiscard]] std::string_view identifier() const noexcept;
};

struct LIQUID_EXPORT signature {
    std::string algorithm;
    std::string signing_key_identifier;
    std::string signing_key_label;
    std::string signature_purpose;
    std::string signed_at;
    std::vector<std::byte> bytes;
};

struct LIQUID_EXPORT compromise_declaration {
    std::string key_fingerprint_base32;
    std::string declared_at;
    std::optional<std::string> reason;
};

struct LIQUID_EXPORT identity_document {
    actor subject;
    std::string key_label;
    std::string created_at;
    std::string updated_at;
    std::optional<std::vector<std::byte>> previous_document_hash;
    std::vector<compromise_declaration> compromised_keys;
    std::vector<signature> signatures;
};

struct LIQUID_EXPORT verified_identity {
    actor subject;
    std::vector<actor> current_keys;
    bool has_compromised_history{};
};

class LIQUID_EXPORT signer {
public:
    signer() noexcept;
    ~signer();
    signer(signer&&) noexcept;
    signer& operator=(signer&&) noexcept;
    signer(const signer&) = delete;
    signer& operator=(const signer&) = delete;

    [[nodiscard]] static runtime::task<signer> open(std::string identifier, keyring& keys);
    [[nodiscard]] runtime::task<signature> sign(std::string artifact_type,
                                                std::vector<std::byte> canonical_body);
    [[nodiscard]] runtime::task<identity_document>
    rotate(std::optional<compromise_declaration> compromise = {});

    [[nodiscard]] actor identity() const;

private:
    friend runtime::task<signer> identity_detail::local_signer();
    struct impl;
    std::unique_ptr<impl> impl_;
};

[[nodiscard]] LIQUID_EXPORT result<bool> verify(const actor& by,
                                                std::string_view artifact_type,
                                                std::span<const std::byte> canonical_body,
                                                const signature& signed_value);

[[nodiscard]] LIQUID_EXPORT result<bool> verify_document(const identity_document& document);
[[nodiscard]] LIQUID_EXPORT result<verified_identity>
verify_chain(std::span<const identity_document> documents);

struct LIQUID_EXPORT argon2_parameters {
    std::uint32_t memory_kib{64U * 1024U};
    std::uint32_t iterations{3};
    std::uint32_t lanes{1};
};

class LIQUID_EXPORT key_provider {
public:
    key_provider() = default;
    virtual ~key_provider() = default;
    key_provider(key_provider&&) noexcept = default;
    key_provider& operator=(key_provider&&) noexcept = default;
    key_provider(const key_provider&) = delete;
    key_provider& operator=(const key_provider&) = delete;

protected:
    [[nodiscard]] virtual result<std::array<std::byte, 32>>
    derive_kek(std::span<const std::byte> salt, argon2_parameters parameters) const = 0;

    friend class keyring;
};

class LIQUID_EXPORT passphrase_provider final : public key_provider {
public:
    explicit passphrase_provider(std::string passphrase);
    ~passphrase_provider() override;
    passphrase_provider(passphrase_provider&&) noexcept;
    passphrase_provider& operator=(passphrase_provider&&) noexcept;
    passphrase_provider(const passphrase_provider&) = delete;
    passphrase_provider& operator=(const passphrase_provider&) = delete;

    [[nodiscard]] result<std::array<std::byte, 32>> derive(std::span<const std::byte> salt,
                                                           argon2_parameters parameters = {}) const;

protected:
    [[nodiscard]] result<std::array<std::byte, 32>>
    derive_kek(std::span<const std::byte> salt, argon2_parameters parameters) const override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT key_file_provider final : public key_provider {
public:
    [[nodiscard]] static runtime::task<std::shared_ptr<key_file_provider>>
    open(std::filesystem::path path);

    ~key_file_provider() override;
    key_file_provider(key_file_provider&&) noexcept;
    key_file_provider& operator=(key_file_provider&&) noexcept;
    key_file_provider(const key_file_provider&) = delete;
    key_file_provider& operator=(const key_file_provider&) = delete;

protected:
    [[nodiscard]] result<std::array<std::byte, 32>>
    derive_kek(std::span<const std::byte> salt, argon2_parameters parameters) const override;

private:
    explicit key_file_provider(std::array<std::byte, 32> key) noexcept;
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT keyring {
public:
    keyring();
    ~keyring();
    keyring(keyring&&) noexcept;
    keyring& operator=(keyring&&) noexcept;
    keyring(const keyring&) = delete;
    keyring& operator=(const keyring&) = delete;

    [[nodiscard]] result<void> add(std::string key_id, std::shared_ptr<key_provider> provider);
    [[nodiscard]] result<std::string> default_key_id() const;
    [[nodiscard]] result<void> set_default_key_id(std::string key_id);
    [[nodiscard]] runtime::task<void> rewrap(std::string key_id,
                                             std::shared_ptr<key_provider> new_provider);

private:
    [[nodiscard]] runtime::task<void> attach(std::filesystem::path keys_path);
    [[nodiscard]] runtime::task<std::array<std::byte, 32>> data_key(std::string key_id,
                                                                    bool create_if_missing);

    struct impl;
    std::unique_ptr<impl> impl_;

    friend class store_detail::store_backend;
};

} // namespace liquid
