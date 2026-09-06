#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid::asn1 {

enum class tag_class : std::uint8_t {
    universal = 0,
    application = 1,
    context_specific = 2,
    private_use = 3,
};

struct tag {
    tag_class kind = tag_class::universal;
    std::uint32_t number = 0;
    bool constructed = false;

    friend auto operator==(const tag&, const tag&) -> bool = default;
};

struct element {
    tag identifier;
    std::span<const std::byte> encoded;
    std::span<const std::byte> content;
};

class LIQUID_EXPORT reader {
public:
    explicit reader(std::span<const std::byte> input) noexcept;

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto remaining() const noexcept -> std::span<const std::byte>;
    auto next() -> result<std::optional<element>>;
    auto single() -> result<element>;

private:
    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
};

LIQUID_EXPORT auto unsigned_integer(const element& value) -> result<std::span<const std::byte>>;
LIQUID_EXPORT auto object_identifier(const element& value) -> result<std::string>;

} // namespace liquid::asn1
