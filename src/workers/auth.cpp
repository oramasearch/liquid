#include "workers/auth.hpp"

#include <array>
#include <utility>

#include "crypto/crypto.hpp"
#include "identity/taap.hpp"

namespace liquid::workers {
namespace {

auto unauthorized(std::string message, std::string context = {}) -> error {
    return {errc::unauthorized, std::move(message), std::move(context)};
}

template <typename Message> auto unsigned_body(Message value) -> result<std::vector<std::byte>> {
    value.signature.clear();
    return encode(value);
}

} // namespace

auto random_token() -> result<std::string> {
    auto bytes = std::array<std::byte, 16>{};
    auto filled = crypto_detail::random_bytes(bytes);
    if (!filled) {
        return std::unexpected(filled.error());
    }
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

auto unix_milliseconds(std::chrono::system_clock::time_point at) -> std::uint64_t {
    const auto count =
        std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    return count < 0 ? 0U : static_cast<std::uint64_t>(count);
}

auto signing_body(const worker_request& value) -> result<std::vector<std::byte>> {
    return unsigned_body(value);
}

auto signing_body(const heartbeat_request& value) -> result<std::vector<std::byte>> {
    return unsigned_body(value);
}

auto signing_body(const complete_request& value) -> result<std::vector<std::byte>> {
    return unsigned_body(value);
}

auto signing_body(const lease_response& value) -> result<std::vector<std::byte>> {
    return unsigned_body(value);
}

auto pinned_key(const identity_document& document) -> result<actor> {
    auto verified = verify_document(document);
    if (!verified || !*verified) {
        return std::unexpected(
            unauthorized("pinned identity document does not verify", document.subject.taap_id));
    }
    return document.subject;
}

auto verify_pinned(const actor& pinned,
                   std::string_view purpose,
                   std::span<const std::byte> body,
                   std::span<const std::byte> signature) -> result<void> {
    if (signature.empty()) {
        return std::unexpected(
            unauthorized("worker protocol message is not signed", std::string{purpose}));
    }
    auto parsed = identity_detail::parse_signature(signature);
    if (!parsed) {
        return std::unexpected(
            unauthorized("worker protocol signature is malformed", std::string{purpose}));
    }
    auto verified = identity_detail::verify_with_pinned_key(pinned, purpose, body, *parsed);
    if (!verified || !*verified) {
        return std::unexpected(unauthorized("worker protocol signature does not verify",
                                            std::string{purpose} + " " + pinned.taap_id));
    }
    return {};
}

auto verify_lease(const lease_response& lease,
                  const actor& scheduler,
                  std::string_view worker,
                  std::string_view nonce) -> result<void> {
    auto body = signing_body(lease);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto verified = verify_pinned(scheduler, lease_purpose, *body, lease.signature);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    if (lease.worker != worker || nonce.empty() || lease.nonce != nonce) {
        return std::unexpected(
            unauthorized("lease does not answer this worker's request", std::string{worker}));
    }
    if (lease.assigned && lease.token.size() != 32) {
        return std::unexpected(unauthorized("lease token is not 128 bits", lease.operation));
    }
    return {};
}

} // namespace liquid::workers
