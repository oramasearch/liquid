#include "liquid/error.hpp"

namespace liquid {

std::string_view to_string(errc code) noexcept {
    switch (code) {
    case errc::ok:
        return "ok";
    case errc::not_found:
        return "not_found";
    case errc::already_exists:
        return "already_exists";
    case errc::conflict:
        return "conflict";
    case errc::invalid_argument:
        return "invalid_argument";
    case errc::unsupported:
        return "unsupported";
    case errc::corrupt:
        return "corrupt";
    case errc::io:
        return "io";
    case errc::locked:
        return "locked";
    case errc::unauthorized:
        return "unauthorized";
    case errc::unavailable:
        return "unavailable";
    case errc::busy:
        return "busy";
    case errc::internal:
        return "internal";
    case errc::not_remote:
        return "not_remote";
    case errc::not_configured:
        return "not_configured";
    }
    return "unknown";
}

} // namespace liquid
