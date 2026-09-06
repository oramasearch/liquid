#include <format>
#include <print>
#include <string>
#include <string_view>

namespace liquid_cli {

auto tier_line(unsigned level, std::string_view mechanism) -> std::string {
    return std::format("tier: {} ({})", level, mechanism);
}

auto tier_json(unsigned level, std::string_view mechanism) -> std::string {
    auto escaped = std::string{};
    escaped.reserve(mechanism.size());
    for (const auto character : mechanism) {
        if (character == '\\' || character == '"') {
            escaped += '\\';
        }
        escaped += character;
    }
    return std::format("{{\"level\":{},\"mechanism\":\"{}\"}}", level, escaped);
}

auto print_tier(unsigned level, std::string_view mechanism) -> void {
    std::println("{}", tier_line(level, mechanism));
}

auto wrap_tier_mechanism() -> std::string_view {
#if defined(__linux__)
    return "wrap: Linux namespaces";
#elif defined(__APPLE__)
    return "wrap: macOS sandbox-exec";
#elif defined(_WIN32)
    return "wrap: Windows unsupported";
#else
    return "wrap: platform unsupported";
#endif
}

} // namespace liquid_cli
