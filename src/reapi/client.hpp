// SPDX-License-Identifier: MIT

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/remote.hpp"

namespace liquid::reapi {

class grpc_client {
public:
    explicit grpc_client(remote_config config);

    [[nodiscard]] runtime::task<std::vector<std::vector<std::byte>>>
    call(std::string_view method, std::span<const std::byte> request, bool streaming = false);
    [[nodiscard]] const remote_config& config() const noexcept;

private:
    remote_config config_;

    [[nodiscard]] runtime::task<std::vector<std::vector<std::byte>>>
    call_unbounded(std::string method, std::vector<std::byte> request);
};

} // namespace liquid::reapi
