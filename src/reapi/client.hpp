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
    // A client-streaming call: every request is sent as its own gRPC message on one stream. A
    // server reads such a stream one message at a time (liquid#451), so a large upload is sent
    // as many bounded messages rather than one.
    [[nodiscard]] runtime::task<std::vector<std::vector<std::byte>>>
    call_messages(std::string_view method, std::vector<std::vector<std::byte>> requests);
    [[nodiscard]] const remote_config& config() const noexcept;

private:
    remote_config config_;

    [[nodiscard]] runtime::task<std::vector<std::vector<std::byte>>>
    call_unbounded(std::string method, std::vector<std::vector<std::byte>> requests);
};

} // namespace liquid::reapi
