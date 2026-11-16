// SPDX-License-Identifier: MIT

#include "liquid/asn1.hpp"

#include <limits>
#include <utility>

namespace liquid::asn1 {
namespace {

constexpr auto byte(std::byte value) noexcept -> std::uint8_t {
    return std::to_integer<std::uint8_t>(value);
}

auto invalid(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "DER"};
}

auto append_component(std::string& output, std::uint64_t value) -> void {
    if (!output.empty()) {
        output.push_back('.');
    }
    output += std::to_string(value);
}

} // namespace

reader::reader(std::span<const std::byte> input) noexcept : input_(input) {}

auto reader::empty() const noexcept -> bool {
    return offset_ == input_.size();
}

auto reader::remaining() const noexcept -> std::span<const std::byte> {
    return input_.subspan(offset_);
}

auto reader::next() -> result<std::optional<element>> {
    if (empty()) {
        return std::nullopt;
    }

    const std::size_t start = offset_;
    const std::uint8_t first = byte(input_[offset_++]);
    tag identifier{
        .kind = static_cast<tag_class>(first >> 6),
        .number = static_cast<std::uint32_t>(first & 0x1f),
        .constructed = (first & 0x20) != 0,
    };

    if (identifier.number == 0x1f) {
        identifier.number = 0;
        bool first_component = true;
        bool finished = false;
        while (offset_ < input_.size()) {
            const std::uint8_t component = byte(input_[offset_++]);
            if (first_component && (component & 0x7f) == 0) {
                return std::unexpected(invalid("non-minimal high-tag-number encoding"));
            }
            first_component = false;
            if (identifier.number > (std::numeric_limits<std::uint32_t>::max() >> 7)) {
                return std::unexpected(invalid("tag number is too large"));
            }
            identifier.number =
                static_cast<std::uint32_t>((identifier.number << 7) | (component & 0x7f));
            if ((component & 0x80) == 0) {
                finished = true;
                break;
            }
        }
        if (!finished) {
            return std::unexpected(invalid("truncated high-tag-number encoding"));
        }
        if (identifier.number < 31) {
            return std::unexpected(invalid("non-minimal high-tag-number encoding"));
        }
    }

    if (offset_ == input_.size()) {
        return std::unexpected(invalid("missing length"));
    }
    const std::uint8_t first_length = byte(input_[offset_++]);
    std::size_t length = 0;
    if ((first_length & 0x80) == 0) {
        length = first_length;
    } else {
        const std::size_t length_octets = first_length & 0x7f;
        if (length_octets == 0) {
            return std::unexpected(invalid("indefinite lengths are not DER"));
        }
        if (length_octets > sizeof(std::size_t) || length_octets > input_.size() - offset_) {
            return std::unexpected(invalid("invalid long-form length"));
        }
        if (byte(input_[offset_]) == 0) {
            return std::unexpected(invalid("non-minimal long-form length"));
        }
        for (std::size_t index = 0; index < length_octets; ++index) {
            if (length > (std::numeric_limits<std::size_t>::max() >> 8)) {
                return std::unexpected(invalid("content length is too large"));
            }
            length = (length << 8) | byte(input_[offset_++]);
        }
        if (length < 128) {
            return std::unexpected(invalid("long-form length used for short content"));
        }
    }
    if (length > input_.size() - offset_) {
        return std::unexpected(invalid("content is truncated"));
    }

    const std::size_t content_start = offset_;
    offset_ += length;
    return element{
        .identifier = identifier,
        .encoded = input_.subspan(start, offset_ - start),
        .content = input_.subspan(content_start, length),
    };
}

auto reader::single() -> result<element> {
    auto value = next();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(invalid("expected one element"));
    }
    if (!empty()) {
        return std::unexpected(invalid("trailing data after element"));
    }
    return **value;
}

auto unsigned_integer(const element& value) -> result<std::span<const std::byte>> {
    if (value.identifier != tag{tag_class::universal, 2, false}) {
        return std::unexpected(invalid("expected INTEGER"));
    }
    if (value.content.empty()) {
        return std::unexpected(invalid("INTEGER is empty"));
    }
    if ((byte(value.content.front()) & 0x80) != 0) {
        return std::unexpected(invalid("INTEGER is negative"));
    }
    if (value.content.size() > 1 && value.content.front() == std::byte{0} &&
        (byte(value.content[1]) & 0x80) == 0) {
        return std::unexpected(invalid("INTEGER has redundant leading zero"));
    }
    if (value.content.size() > 1 && value.content.front() == std::byte{0}) {
        return value.content.subspan(1);
    }
    return value.content;
}

auto object_identifier(const element& value) -> result<std::string> {
    if (value.identifier != tag{tag_class::universal, 6, false}) {
        return std::unexpected(invalid("expected OBJECT IDENTIFIER"));
    }
    if (value.content.empty()) {
        return std::unexpected(invalid("OBJECT IDENTIFIER is empty"));
    }

    std::string output;
    std::uint64_t component = 0;
    bool component_start = true;
    bool first = true;
    for (const std::byte encoded : value.content) {
        const std::uint8_t current = byte(encoded);
        if (component_start && current == 0x80) {
            return std::unexpected(invalid("OBJECT IDENTIFIER component is not minimal"));
        }
        component_start = false;
        if (component > (std::numeric_limits<std::uint64_t>::max() >> 7)) {
            return std::unexpected(invalid("OBJECT IDENTIFIER component is too large"));
        }
        component = (component << 7) | (current & 0x7f);
        if ((current & 0x80) != 0) {
            continue;
        }
        if (first) {
            const std::uint64_t first_component = component < 40 ? 0 : component < 80 ? 1 : 2;
            append_component(output, first_component);
            append_component(output, component - first_component * 40);
            first = false;
        } else {
            append_component(output, component);
        }
        component = 0;
        component_start = true;
    }
    if (!component_start) {
        return std::unexpected(invalid("OBJECT IDENTIFIER is truncated"));
    }
    return output;
}

} // namespace liquid::asn1
