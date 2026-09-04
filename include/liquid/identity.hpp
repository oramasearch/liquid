#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

namespace store_detail {
class store_backend;
}

struct LIQUID_EXPORT actor {
    std::string taap_id;
};

class LIQUID_EXPORT signer {
public:
    signer() noexcept;
    ~signer();
    signer(signer&&) noexcept;
    signer& operator=(signer&&) noexcept;
    signer(const signer&) = delete;
    signer& operator=(const signer&) = delete;

    [[nodiscard]] runtime::task<std::vector<std::byte>> sign(std::vector<std::byte> payload);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

[[nodiscard]] LIQUID_EXPORT result<bool> verify(
    const actor& by, std::span<const std::byte> payload,
    std::span<const std::byte> signature);

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
    [[nodiscard]] virtual result<std::array<std::byte, 32>> derive_kek(
        std::span<const std::byte> salt, argon2_parameters parameters) const = 0;

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

    [[nodiscard]] result<std::array<std::byte, 32>> derive(
        std::span<const std::byte> salt, argon2_parameters parameters = {}) const;

protected:
    [[nodiscard]] result<std::array<std::byte, 32>> derive_kek(
        std::span<const std::byte> salt, argon2_parameters parameters) const override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT key_file_provider final : public key_provider {
public:
    [[nodiscard]] static runtime::task<std::shared_ptr<key_file_provider>> open(
        std::filesystem::path path);

    ~key_file_provider() override;
    key_file_provider(key_file_provider&&) noexcept;
    key_file_provider& operator=(key_file_provider&&) noexcept;
    key_file_provider(const key_file_provider&) = delete;
    key_file_provider& operator=(const key_file_provider&) = delete;

protected:
    [[nodiscard]] result<std::array<std::byte, 32>> derive_kek(
        std::span<const std::byte> salt, argon2_parameters parameters) const override;

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
    [[nodiscard]] runtime::task<void> rewrap(
        std::string key_id, std::shared_ptr<key_provider> new_provider);

private:
    [[nodiscard]] runtime::task<void> attach(std::filesystem::path keys_path);
    [[nodiscard]] runtime::task<std::array<std::byte, 32>> data_key(
        std::string key_id, bool create_if_missing);

    struct impl;
    std::unique_ptr<impl> impl_;

    friend class store_detail::store_backend;
};

}  // namespace liquid
