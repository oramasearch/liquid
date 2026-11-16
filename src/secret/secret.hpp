// SPDX-License-Identifier: MIT

#pragma once

#include <functional>

#include "liquid/secret.hpp"
#include "net/http_client.hpp"

namespace liquid::secret_detail {

using random_source = std::function<result<void>(std::span<std::byte>)>;
using http_requester =
    std::function<runtime::task<net_detail::http_response>(net_detail::http_request)>;

[[nodiscard]] LIQUID_EXPORT auto seal_secret_with_random(const sealing_public_key& recipient,
                                                         seal_scope scope,
                                                         std::span<const std::byte> plaintext,
                                                         const random_source& random)
    -> result<sealed_secret>;
[[nodiscard]] LIQUID_EXPORT auto fetch_sealing_public_key_with(std::string lake_url,
                                                               std::string bearer,
                                                               const http_requester& request)
    -> runtime::task<sealing_public_key>;

} // namespace liquid::secret_detail
