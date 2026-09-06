#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>

#include "liquid/attestation.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto read_attestation(const liquid::digest& id) -> liquid::result<liquid::attestation> {
    const auto root = std::filesystem::current_path();
    const auto read =
        [&id](const std::filesystem::path& path) -> liquid::result<liquid::attestation> {
        auto store = liquid::runtime::block_on(liquid::store::open(path));
        if (!store) {
            return std::unexpected(store.error());
        }
        auto bytes = liquid::runtime::block_on(store->get(id));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return liquid::decode_attestation(*bytes);
    };
    const auto primary = root / ".liquid-store";
    if (std::filesystem::exists(primary)) {
        return read(primary);
    }
    const auto capture = root / ".liquid" / "capture" / "store";
    if (std::filesystem::exists(capture)) {
        return read(capture);
    }
    return std::unexpected(liquid::error{liquid::errc::not_found,
                                         "no repository or capture store in the current directory",
                                         root.string()});
}

auto attribute_text(const liquid::attestation_value& value) -> std::string {
    return std::visit(
        [](const auto& selected) -> std::string {
            using value_type = std::decay_t<decltype(selected)>;
            if constexpr (std::is_same_v<value_type, std::string>) {
                return selected;
            } else if constexpr (std::is_same_v<value_type, bool>) {
                return selected ? "true" : "false";
            } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
                return std::to_string(selected);
            } else {
                auto output = std::string{};
                for (std::size_t index = 0; index != selected.size(); ++index) {
                    output += index == 0 ? "" : ",";
                    output += selected[index];
                }
                return output;
            }
        },
        value);
}

} // namespace

auto attestation(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "image") {
        if (argc < 4) {
            return usage(
                "attest image <ref@sha256:...> --tree <digest> [--result <id>] [--store <path>]");
        }
        auto image = std::string{argv[1]};
        auto tree = std::optional<liquid::digest>{};
        auto result = std::optional<liquid::digest>{};
        auto destination = std::filesystem::path{};
        for (auto index = 2; index < argc; ++index) {
            const auto argument = std::string_view{argv[index]};
            if (argument == "--tree" && ++index < argc) {
                auto parsed = liquid::parse_digest(argv[index]);
                if (!parsed) {
                    return fail(parsed.error());
                }
                tree = *parsed;
            } else if (argument == "--result" && ++index < argc) {
                auto parsed = liquid::parse_digest(argv[index]);
                if (!parsed) {
                    return fail(parsed.error());
                }
                result = *parsed;
            } else if (argument == "--store" && ++index < argc) {
                destination = argv[index];
            } else {
                return usage("attest image received an unknown or incomplete option");
            }
        }
        if (!tree) {
            return usage("attest image requires --tree <digest>");
        }
        if (destination.empty()) {
            destination = std::filesystem::current_path() / ".liquid-store";
            if (!std::filesystem::exists(destination)) {
                return usage("attest image must run from a liquid repository or use --store");
            }
        }
        auto stored = liquid::runtime::block_on(
            liquid::attest_image(std::move(image), *tree, result, std::move(destination)));
        if (!stored) {
            return fail(stored.error());
        }
        std::println("attestation={}", liquid::to_string(*stored));
        return 0;
    }
    if (argc != 2 ||
        (std::string_view{argv[0]} != "show" && std::string_view{argv[0]} != "verify")) {
        return usage("attest requires show|verify and a digest, or image and its provenance");
    }
    auto id = liquid::parse_digest(argv[1]);
    if (!id) {
        return fail(id.error());
    }
    auto value = read_attestation(*id);
    if (!value) {
        return fail(value.error());
    }
    const auto command = std::string_view{argv[0]};
    if (command == "show") {
        std::println("profile={}", value->profile);
        std::println("subject={}", liquid::to_string(value->subject));
        std::println("subject_kind={}", liquid::to_string(value->subject_kind));
        std::println("issued_at={}", value->at.time_since_epoch().count());
        std::println("issuer={}", value->issuer.taap_id);
        std::println("attributes={}", value->attributes.size());
        for (const auto& attribute : value->attributes) {
            std::println("attribute.{}={}", attribute.name, attribute_text(attribute.value));
        }
        std::println("commitments={}", value->disclosure_commitments.size());
        return 0;
    }
    auto verified = liquid::verify_attestation(*value);
    if (!verified) {
        return fail(verified.error());
    }
    if (!verified->verified) {
        std::println(stderr, "error: invalid attestation: {}", verified->reason);
        return 1;
    }
    std::println("verified {}", liquid::to_string(*id));
    return 0;
}

} // namespace liquid_cli
