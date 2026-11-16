// SPDX-License-Identifier: MIT

#include "liquid/listener.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>

#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include "liquid/hash.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liquid {
namespace {

constexpr auto maximum_token_file_bytes = std::uintmax_t{4096};

auto listener_error(std::string message, std::string context = {}) -> error {
    return {errc::invalid_argument, std::move(message), std::move(context)};
}

auto token_character(char value) -> bool {
    return value > ' ' && value < '\x7f';
}

auto ascii_lower(char value) -> char {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

auto token_bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

} // namespace

bool is_loopback_address(std::string_view host) noexcept {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    auto v4 = boost::corosio::ipv4_address{};
    if (!boost::corosio::parse_ipv4_address(host, v4)) {
        return v4.is_loopback();
    }
    auto v6 = boost::corosio::ipv6_address{};
    if (!boost::corosio::parse_ipv6_address(host, v6)) {
        return v6.is_loopback();
    }
    return false;
}

result<void> validate_listener_security(bool loopback, const listener_security& security) {
    if (security.tls_certificate.has_value() != security.tls_key.has_value()) {
        return std::unexpected(
            listener_error("a TLS certificate and its private key must be given together"));
    }
    const auto tls = security.tls_certificate.has_value();
    if (security.client_certificate_authority && !tls) {
        return std::unexpected(
            listener_error("a client certificate authority requires a TLS certificate and key"));
    }
    if (!security.bearer_token.empty()) {
        if (security.bearer_token.size() < minimum_bearer_token_bytes) {
            return std::unexpected(listener_error(
                "the bearer token is too short",
                "at least " + std::to_string(minimum_bearer_token_bytes) + " bytes are required"));
        }
        if (!std::ranges::all_of(security.bearer_token, token_character)) {
            return std::unexpected(
                listener_error("the bearer token must be printable ASCII without spaces"));
        }
    }
    if (loopback) {
        return {};
    }
    const auto authenticated =
        !security.bearer_token.empty() || security.client_certificate_authority.has_value();
    if (!tls || !authenticated) {
        return std::unexpected(listener_error(
            "a listener on a non-loopback address requires TLS and client authentication",
            "give --tls-cert and --tls-key, and --token-file or --client-ca; plaintext is served "
            "only on a loopback address"));
    }
    return {};
}

result<std::string> read_token_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    // Owner-only access is an ACL property on Windows, which this check cannot read; fail closed.
    return std::unexpected(listener_error(
        "token files are not supported on Windows; use a client certificate", path.string()));
#else
    auto code = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, code);
    if (code) {
        return std::unexpected(listener_error("cannot read the token file", path.string()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(
            listener_error("the token file must be a regular file", path.string()));
    }
    using std::filesystem::perms;
    if ((status.permissions() & (perms::group_all | perms::others_all)) != perms::none) {
        return std::unexpected(listener_error(
            "the token file must be readable only by its owner (chmod 600)", path.string()));
    }
    struct stat details{};
    if (::lstat(path.c_str(), &details) != 0 || details.st_uid != ::geteuid()) {
        return std::unexpected(listener_error(
            "the token file must be owned by the user running liquid", path.string()));
    }
    const auto size = std::filesystem::file_size(path, code);
    if (code || size > maximum_token_file_bytes) {
        return std::unexpected(listener_error("the token file is too large", path.string()));
    }
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(listener_error("cannot read the token file", path.string()));
    }
    auto token = std::string{std::istreambuf_iterator<char>{input}, {}};
    while (!token.empty() &&
           (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    if (token.empty()) {
        return std::unexpected(listener_error("the token file is empty", path.string()));
    }
    return token;
#endif
}

bool bearer_authorized(std::string_view expected, std::string_view authorization) noexcept {
    if (expected.empty()) {
        return false;
    }
    constexpr auto scheme = std::string_view{"bearer "};
    if (authorization.size() <= scheme.size()) {
        return false;
    }
    for (auto index = std::size_t{}; index < scheme.size(); ++index) {
        if (ascii_lower(authorization[index]) != scheme[index]) {
            return false;
        }
    }
    const auto presented = authorization.substr(scheme.size());
    try {
        const auto expected_digest = hash(algorithm::sha256, token_bytes(expected));
        const auto presented_digest = hash(algorithm::sha256, token_bytes(presented));
        if (!expected_digest || !presented_digest) {
            return false;
        }
        auto difference = std::uint8_t{};
        for (auto index = std::size_t{}; index < expected_digest->bytes.size(); ++index) {
            difference |= std::to_integer<std::uint8_t>(expected_digest->bytes[index] ^
                                                        presented_digest->bytes[index]);
        }
        return difference == 0;
    } catch (...) {
        return false;
    }
}

} // namespace liquid
