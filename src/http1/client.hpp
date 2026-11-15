#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid::http1 {

struct response {
    unsigned status{};
    std::vector<std::byte> body;
};

struct request_options {
    std::string url;
    std::string method{"GET"};
    std::string suffix;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::byte> body;
    bool verify_peer{true};
    // The request carries something a network attacker must not read or change (a key, a
    // credential, or the answer it is trusted for). It and any request with an Authorization
    // header go over plain http only where plaintext_allowed says so (L-10, liquid#392).
    bool sensitive{false};
};

// Whether plain http may reach `host` with a sensitive request: loopback, a Kubernetes Service
// inside the cluster (`*.svc`, `*.svc.cluster.local`), or anywhere with
// LIQUID_ALLOW_INSECURE_HTTP=1 (liquid#392).
[[nodiscard]] LIQUID_EXPORT auto plaintext_allowed(std::string_view host) -> bool;

[[nodiscard]] auto validate_url(std::string_view url) -> result<void>;
[[nodiscard]] LIQUID_EXPORT auto request(request_options options) -> runtime::task<response>;

} // namespace liquid::http1
