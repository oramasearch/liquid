#pragma once

#include <filesystem>
#include <string>

#include "liquid/export.hpp"
#include "liquid/listener.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT reapi_serve_options {
    std::filesystem::path store_directory;
    std::string listen{"127.0.0.1:50051"};
    std::string instance_name;
    // TLS and client authentication (liquid#415). `serve_reapi` refuses a non-loopback `listen`
    // unless this has TLS and a bearer token or a client certificate authority. A configured
    // bearer token is required on every RPC, reads included.
    listener_security security;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_reapi(reapi_serve_options options);

} // namespace liquid
