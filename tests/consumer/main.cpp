#include <cstddef>
#include <span>

#include <nlohmann/json.hpp>

#include "liquid/identity.hpp"

auto main() -> int {
    const auto marker = nlohmann::json{{"consumer", "lake-shaped"}};
    const auto decoded = liquid::decode_identity_document(std::span<const std::byte>{});
    return !decoded && marker.at("consumer") == "lake-shaped" ? 0 : 1;
}
