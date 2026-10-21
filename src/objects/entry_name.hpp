#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "liquid/error.hpp"

namespace liquid::objects {

// The rule every tree entry name obeys (liquid #386). A name is one path component: it is not
// empty, `.` or `..`, contains no `/`, `\` or NUL, and is not a spelling of `.git` that Git,
// macOS (HFS+ ignorable code points) or Windows (8.3 short names, trailing dots and spaces)
// would treat as `.git`. Returns the reason the name is refused, or nothing when it is valid.
[[nodiscard]] auto entry_name_problem(std::string_view name) -> std::optional<std::string_view>;

// `entry_name_problem` as an error with `code`, the reason as its message and the name, with
// control bytes escaped, as its context.
[[nodiscard]] auto validate_entry_name(std::string_view name, errc code) -> result<void>;

// The name with control and non-ASCII bytes written as `\xNN`, for diagnostics.
[[nodiscard]] auto printable_entry_name(std::string_view name) -> std::string;

} // namespace liquid::objects
