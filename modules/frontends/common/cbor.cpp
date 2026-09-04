#include "cbor.hpp"

#include <algorithm>
#include <limits>

namespace liquid_frontend {
namespace {

void append_unsigned(std::vector<std::uint8_t>& output, std::uint8_t major, std::uint64_t value) {
    const auto initial = static_cast<std::uint8_t>(major << 5U);
    if (value < 24) {
        output.push_back(static_cast<std::uint8_t>(initial | value));
        return;
    }
    if (value <= std::numeric_limits<std::uint8_t>::max()) {
        output.push_back(static_cast<std::uint8_t>(initial | 24U));
        output.push_back(static_cast<std::uint8_t>(value));
        return;
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        output.push_back(static_cast<std::uint8_t>(initial | 25U));
        output.push_back(static_cast<std::uint8_t>(value >> 8U));
        output.push_back(static_cast<std::uint8_t>(value));
        return;
    }
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
        output.push_back(static_cast<std::uint8_t>(initial | 26U));
        for (const auto shift : {24U, 16U, 8U, 0U}) {
            output.push_back(static_cast<std::uint8_t>(value >> shift));
        }
        return;
    }
    output.push_back(static_cast<std::uint8_t>(initial | 27U));
    for (const auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U}) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void encode_value(const cbor_value& value, std::vector<std::uint8_t>& output) {
    std::visit(
        [&](const auto& item) {
            using type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<type, std::uint64_t>) {
                append_unsigned(output, 0, item);
            } else if constexpr (std::is_same_v<type, cbor_value::bytes>) {
                append_unsigned(output, 2, item.size());
                output.insert(output.end(), item.begin(), item.end());
            } else if constexpr (std::is_same_v<type, std::string>) {
                append_unsigned(output, 3, item.size());
                output.insert(output.end(), item.begin(), item.end());
            } else if constexpr (std::is_same_v<type, cbor_value::array>) {
                append_unsigned(output, 4, item.size());
                for (const auto& child : item) {
                    encode_value(child, output);
                }
            } else {
                struct encoded_entry {
                    std::vector<std::uint8_t> key;
                    const cbor_value* value{};
                };
                auto entries = std::vector<encoded_entry>{};
                entries.reserve(item.size());
                for (const auto& [key, child] : item) {
                    auto encoded_key = std::vector<std::uint8_t>{};
                    encode_value(cbor_value{key}, encoded_key);
                    entries.push_back({std::move(encoded_key), &child});
                }
                std::ranges::sort(entries, {}, &encoded_entry::key);
                append_unsigned(output, 5, entries.size());
                for (const auto& entry : entries) {
                    output.insert(output.end(), entry.key.begin(), entry.key.end());
                    encode_value(*entry.value, output);
                }
            }
        },
        value.data);
}

class decoder {
public:
    explicit decoder(std::span<const std::uint8_t> input) : input_(input) {}

    auto decode() -> std::optional<cbor_value> {
        auto value = read();
        if (!value || position_ != input_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    auto unsigned_value(std::uint8_t additional) -> std::optional<std::uint64_t> {
        if (additional < 24) {
            return additional;
        }
        auto width = std::size_t{};
        if (additional == 24) {
            width = 1;
        } else if (additional == 25) {
            width = 2;
        } else if (additional == 26) {
            width = 4;
        } else if (additional == 27) {
            width = 8;
        } else {
            return std::nullopt;
        }
        if (input_.size() - position_ < width) {
            return std::nullopt;
        }
        auto value = std::uint64_t{};
        for (std::size_t index = 0; index < width; ++index) {
            value = (value << 8U) | input_[position_++];
        }
        return value;
    }

    auto read() -> std::optional<cbor_value> {
        if (position_ == input_.size()) {
            return std::nullopt;
        }
        const auto initial = input_[position_++];
        const auto major = static_cast<std::uint8_t>(initial >> 5U);
        auto size = unsigned_value(static_cast<std::uint8_t>(initial & 0x1fU));
        if (!size) {
            return std::nullopt;
        }
        if (major == 0) {
            return cbor_value{*size};
        }
        if (major == 2 || major == 3) {
            if (*size > input_.size() - position_) {
                return std::nullopt;
            }
            const auto begin = input_.begin() + static_cast<std::ptrdiff_t>(position_);
            const auto end = begin + static_cast<std::ptrdiff_t>(*size);
            position_ += static_cast<std::size_t>(*size);
            if (major == 2) {
                return cbor_value{cbor_value::bytes{begin, end}};
            }
            return cbor_value{std::string{begin, end}};
        }
        if (major == 4) {
            auto output = cbor_value::array{};
            output.reserve(static_cast<std::size_t>(*size));
            for (std::uint64_t index = 0; index < *size; ++index) {
                auto child = read();
                if (!child) {
                    return std::nullopt;
                }
                output.push_back(std::move(*child));
            }
            return cbor_value{std::move(output)};
        }
        if (major == 5) {
            auto output = cbor_value::map{};
            output.reserve(static_cast<std::size_t>(*size));
            for (std::uint64_t index = 0; index < *size; ++index) {
                auto key = read();
                auto child = read();
                if (!key || !child) {
                    return std::nullopt;
                }
                const auto* text = std::get_if<std::string>(&key->data);
                if (!text) {
                    return std::nullopt;
                }
                output.emplace_back(*text, std::move(*child));
            }
            return cbor_value{std::move(output)};
        }
        return std::nullopt;
    }

    std::span<const std::uint8_t> input_;
    std::size_t position_{};
};

} // namespace

auto cbor_encode(const cbor_value& value) -> std::vector<std::uint8_t> {
    auto output = std::vector<std::uint8_t>{};
    encode_value(value, output);
    return output;
}

auto cbor_decode(std::span<const std::uint8_t> bytes) -> std::optional<cbor_value> {
    return decoder{bytes}.decode();
}

auto cbor_find(const cbor_value& value, std::string_view key) -> const cbor_value* {
    const auto* object = std::get_if<cbor_value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(*object, key, &cbor_value::map::value_type::first);
    return found == object->end() ? nullptr : &found->second;
}

} // namespace liquid_frontend
