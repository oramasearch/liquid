// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace liquid_frontend {

struct cbor_value {
    using bytes = std::vector<std::uint8_t>;
    using array = std::vector<cbor_value>;
    using map = std::vector<std::pair<std::string, cbor_value>>;
    using storage = std::variant<bool, std::uint64_t, bytes, std::string, array, map>;

    storage data;

    cbor_value(bool value) : data(value) {}
    cbor_value(std::uint64_t value) : data(value) {}
    cbor_value(bytes value) : data(std::move(value)) {}
    cbor_value(std::string value) : data(std::move(value)) {}
    cbor_value(const char* value) : data(std::string{value}) {}
    cbor_value(array value) : data(std::move(value)) {}
    cbor_value(map value) : data(std::move(value)) {}
};

auto cbor_encode(const cbor_value& value) -> std::vector<std::uint8_t>;
auto cbor_decode(std::span<const std::uint8_t> bytes) -> std::optional<cbor_value>;
auto cbor_find(const cbor_value& value, std::string_view key) -> const cbor_value*;

} // namespace liquid_frontend
