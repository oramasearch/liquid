#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "liquid/error.hpp"

namespace liquid::identity_detail {

struct key_reference {
    std::string key_id;
    std::optional<std::uint64_t> version;
};

[[nodiscard]] inline result<key_reference> parse_key_reference(std::string_view value) {
    const auto separator = value.rfind('@');
    auto output = key_reference{std::string{value}, std::nullopt};
    if (separator == std::string_view::npos) {
        return output;
    }
    if (separator == 0 || separator + 1 == value.size()) {
        return std::unexpected(
            error{errc::invalid_argument, "invalid key version", std::string{value}});
    }
    const auto text = value.substr(separator + 1);
    std::uint64_t version = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), version);
    if (code != std::errc{} || end != text.data() + text.size() || version == 0) {
        return std::unexpected(
            error{errc::invalid_argument, "invalid key version", std::string{value}});
    }
    output.key_id = value.substr(0, separator);
    output.version = version;
    return output;
}

[[nodiscard]] inline std::string versioned_key_id(std::string_view key_id, std::uint64_t version) {
    return std::string{key_id} + "@" + std::to_string(version);
}

} // namespace liquid::identity_detail
