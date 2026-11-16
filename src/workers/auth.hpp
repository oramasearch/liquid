// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/identity.hpp"
#include "workers/protocol.hpp"

// Mutual authentication between the scheduler and its workers (liquid #416).
//
// Each side is configured with the other's TAAP identity document, the output of
// `liquid identity show --document`. Every worker request (Lease, Heartbeat, Complete) is signed
// by the worker's key and checked by the scheduler against the pinned document of the worker it
// names; every lease is signed by the scheduler's key and checked by the worker against the pinned
// scheduler document. A request carries a fresh nonce and its issue time, and the scheduler
// refuses a nonce it has already seen or a time outside `max_clock_skew`; a lease names the worker
// and echoes the nonce of the request it answers. Lease tokens are 128 random bits.
namespace liquid::workers {

inline constexpr auto lease_request_purpose = std::string_view{"liquid-worker-lease-request"};
inline constexpr auto heartbeat_purpose = std::string_view{"liquid-worker-heartbeat"};
inline constexpr auto complete_purpose = std::string_view{"liquid-worker-complete"};
inline constexpr auto lease_purpose = std::string_view{"liquid-scheduler-lease"};
inline constexpr auto max_clock_skew = std::chrono::minutes{5};

// 32 lowercase hexadecimal characters from 16 bytes of the operating system's CSPRNG.
[[nodiscard]] auto random_token() -> result<std::string>;

// Milliseconds since the Unix epoch.
[[nodiscard]] auto
unix_milliseconds(std::chrono::system_clock::time_point at = std::chrono::system_clock::now())
    -> std::uint64_t;

// The bytes each signature covers: the message encoded with an empty `signature`.
[[nodiscard]] auto signing_body(const worker_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto signing_body(const heartbeat_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto signing_body(const complete_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto signing_body(const lease_response& value) -> result<std::vector<std::byte>>;

// Verifies the document's self-signature and returns the key it binds to its TAAP identifier.
[[nodiscard]] auto pinned_key(const identity_document& document) -> result<actor>;

// Verifies `signature` (canonical TAAP bytes) over `body` with exactly `pinned`'s key. Any failure,
// including an empty signature, is `errc::unauthorized`.
[[nodiscard]] auto verify_pinned(const actor& pinned,
                                 std::string_view purpose,
                                 std::span<const std::byte> body,
                                 std::span<const std::byte> signature) -> result<void>;

// The worker's check of a Lease response: signed by the pinned scheduler key, naming this worker,
// and answering the request that carried `nonce`. An assigned lease must carry a 128-bit token.
[[nodiscard]] auto verify_lease(const lease_response& lease,
                                const actor& scheduler,
                                std::string_view worker,
                                std::string_view nonce) -> result<void>;

} // namespace liquid::workers
