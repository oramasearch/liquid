// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid::identity_detail {

struct backend_signature {
    std::string algorithm;
    std::vector<std::byte> bytes;
    std::vector<std::byte> authenticator_data;
    std::vector<std::byte> client_data_hash;
};

class key_backend {
public:
    virtual ~key_backend() = default;

    [[nodiscard]] virtual std::string_view algorithm() const noexcept = 0;
    [[nodiscard]] virtual std::string_view custody() const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::byte> public_key() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::byte> descriptor() const = 0;
    [[nodiscard]] virtual runtime::task<backend_signature> sign(std::vector<std::byte> input) = 0;
};

class fido2_transport {
public:
    virtual ~fido2_transport() = default;
    [[nodiscard]] virtual result<std::vector<std::byte>>
    transact(std::uint8_t command, std::span<const std::byte> payload) = 0;
};

class piv_transport {
public:
    virtual ~piv_transport() = default;
    [[nodiscard]] virtual result<std::vector<std::byte>>
    transmit(std::span<const std::byte> apdu) = 0;
};

[[nodiscard]] auto make_software_backend() -> result<std::unique_ptr<key_backend>>;
[[nodiscard]] auto make_software_backend(std::span<const std::byte> secret)
    -> result<std::unique_ptr<key_backend>>;
[[nodiscard]] auto make_fido2_backend() -> runtime::task<std::unique_ptr<key_backend>>;
[[nodiscard]] auto make_piv_backend(std::string slot)
    -> runtime::task<std::unique_ptr<key_backend>>;
[[nodiscard]] auto operating_system_fido2_transport() -> std::shared_ptr<fido2_transport>;
[[nodiscard]] auto open_hardware_backend(std::string custody, std::span<const std::byte> descriptor)
    -> result<std::unique_ptr<key_backend>>;

LIQUID_EXPORT void install_fido2_transport(std::shared_ptr<fido2_transport> transport);
LIQUID_EXPORT void install_piv_transport(std::shared_ptr<piv_transport> transport);

} // namespace liquid::identity_detail
