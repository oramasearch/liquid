// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid {

// Transport security and authentication for a liquid network listener: `serve_reapi` and the
// `liquid serve sync` adapter (liquid#415, liquid#403). A listener bound to a loopback address may
// run without either. A listener on any other address is refused unless it has TLS and at least one
// way to authenticate a client: a bearer token, or a client certificate verified against
// `client_certificate_authority` (mutual TLS). There is no plaintext mode off loopback.
struct LIQUID_EXPORT listener_security {
    // The PEM certificate chain and private key the listener presents. Both, or neither.
    std::optional<std::filesystem::path> tls_certificate;
    std::optional<std::filesystem::path> tls_key;
    // A PEM CA certificate. When set, every client must present a certificate that it signs, or the
    // TLS handshake fails. Requires `tls_certificate`.
    std::optional<std::filesystem::path> client_certificate_authority;
    // When non-empty, every request must carry `Authorization: Bearer <token>`; the comparison is
    // constant-time. At least `minimum_bearer_token_bytes` long, printable ASCII without spaces.
    std::string bearer_token;
};

inline constexpr std::size_t minimum_bearer_token_bytes = 32;

// True only for an IPv4 address in 127.0.0.0/8 or the IPv6 address ::1 (brackets allowed). Host
// names, the unspecified addresses and IPv4-mapped IPv6 addresses are not loopback.
[[nodiscard]] LIQUID_EXPORT bool is_loopback_address(std::string_view host) noexcept;

// Checks `security` for a listener whose address is (`loopback`) or is not a loopback address.
// Returns `errc::invalid_argument` naming what is missing; nothing is opened or read.
[[nodiscard]] LIQUID_EXPORT result<void>
validate_listener_security(bool loopback, const listener_security& security);

// Reads a bearer token from a file only its owner can read or write: a regular file (not a
// symbolic link) with no group or other permission bits, owned by this process's user. Trailing
// line breaks and spaces are removed. The token never travels in argv or the environment. Refused
// on Windows, where owner-only access is an ACL this check cannot read.
[[nodiscard]] LIQUID_EXPORT result<std::string> read_token_file(const std::filesystem::path& path);

// True when `authorization` is `Bearer <token>` (the scheme in any case) and the token equals
// `expected`. Both tokens are hashed first and the digests compared in constant time, so neither
// the content nor the length of the expected token is revealed by timing. False when `expected`
// is empty.
[[nodiscard]] LIQUID_EXPORT bool bearer_authorized(std::string_view expected,
                                                   std::string_view authorization) noexcept;

} // namespace liquid
