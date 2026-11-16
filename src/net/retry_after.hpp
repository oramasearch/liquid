#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "liquid/export.hpp"

namespace liquid::net_detail {

/// The wait a `Retry-After` header field value asks for (RFC 9110 section 10.2.3).
///
/// The value is either delay-seconds (`1*DIGIT`) or an HTTP-date in any of the three forms a
/// recipient must accept: IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`), the obsolete RFC 850 form
/// (`Sunday, 06-Nov-94 08:49:37 GMT`) and the asctime form (`Sun Nov  6 08:49:37 1994`). A date is
/// measured from `now`; a date that has already passed asks for no wait. An empty or malformed
/// value yields `std::nullopt`, which a caller treats as "no retry signal".
[[nodiscard]] LIQUID_EXPORT auto parse_retry_after(std::string_view value,
                                                   std::chrono::system_clock::time_point now)
    -> std::optional<std::chrono::milliseconds>;

} // namespace liquid::net_detail
