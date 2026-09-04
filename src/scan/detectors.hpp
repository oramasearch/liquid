#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/drip.hpp"

namespace liquid::scan_detail {

struct finding {
    std::string detector;
    std::string description;
    std::size_t line{};
};

struct allowed_finding {
    finding value;
    std::string reason;
};

struct report {
    std::vector<std::string> detectors;
    std::vector<finding> blocked;
    std::vector<allowed_finding> allowed;
};

struct byte_range {
    std::size_t offset{};
    std::size_t size{};
};

class detector_set {
public:
    [[nodiscard]] auto scan(std::string_view path,
                            std::span<const std::byte> contents,
                            const scan_configuration& config,
                            std::optional<std::span<const byte_range>> changed = std::nullopt) const
        -> report;
};

[[nodiscard]] auto shannon_entropy(std::string_view value) -> double;
[[nodiscard]] auto glob_matches(std::string_view pattern, std::string_view path) -> bool;

} // namespace liquid::scan_detail
