#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace liquid_cli {

auto repo_log(int argc, char** argv) -> int;

namespace {

auto fail(const liquid::error& value, int status = 1) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return status;
}

auto usage() -> int {
    std::println(
        stderr,
        "error: usage: log [--ops] [--json] | log verify <log-url> <attestation-base64url> | "
        "log consistency <log-url> | log scan <log-url> <identity> | "
        "log trust <log-url> [--confirm <fingerprint>]");
    return 2;
}

auto base64url_decode(std::string_view value) -> liquid::result<std::vector<std::byte>> {
    if (value.empty() || value.size() % 4 == 1) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "attestation is not base64url", {}});
    }
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    auto output = std::vector<std::byte>{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto character : value) {
        const auto position = alphabet.find(character);
        if (position == std::string_view::npos) {
            return std::unexpected(
                liquid::error{liquid::errc::invalid_argument, "attestation is not base64url", {}});
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits < 8) {
            continue;
        }
        bits -= 8;
        output.push_back(std::byte{static_cast<unsigned char>((accumulator >> bits) & 0xffU)});
    }
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

auto print_checkpoint(const liquid::transparency::checkpoint_trust& value) -> void {
    std::println("checkpoint_sequence={} tree_size={} bootstrap_warning={}",
                 value.value.sequence,
                 value.value.tree_size,
                 value.bootstrap_warning);
}

// Verification never takes the log's key or witness policy from the log: it reads the pin from
// $LIQUID_HOME/trust/transparency/ and refuses when there is none (liquid#389).
auto load_pin(std::string_view log_url) -> liquid::result<liquid::transparency::log_trust> {
    return liquid::runtime::block_on(liquid::transparency::load_log_trust(std::string{log_url}));
}

// Trust on first use, only on request and only with the operator's confirmation: without
// --confirm it shows what the log claims and pins nothing. With it, it pins only when the log
// still presents exactly the confirmed key, witnesses and threshold.
auto trust_log(std::string_view log_url, std::string_view confirmed) -> int {
    namespace transparency = liquid::transparency;
    auto source = transparency::http_log_source::create(std::string{log_url});
    if (!source) {
        return fail(source.error(), 2);
    }
    auto existing = load_pin(log_url);
    if (existing) {
        auto path = transparency::log_trust_path(log_url);
        return fail(liquid::error{liquid::errc::already_exists,
                                  "this log is already pinned; remove the pin file to pin it again",
                                  path ? path->string() : std::string{log_url}});
    }
    if (existing.error().code != liquid::errc::not_configured) {
        return fail(existing.error());
    }
    auto identity = liquid::runtime::block_on(source->identity());
    if (!identity) {
        return fail(identity.error());
    }
    auto configuration = liquid::runtime::block_on(source->configuration());
    if (!configuration) {
        return fail(configuration.error());
    }
    auto candidate = transparency::log_trust{std::move(*identity), std::move(*configuration)};
    auto text = transparency::format_log_trust(log_url, candidate);
    if (!text) {
        return fail(text.error());
    }
    auto client = transparency::log_client{*source};
    auto current = liquid::runtime::block_on(client.current_checkpoint(candidate));
    if (!current) {
        return fail(current.error());
    }
    const auto* first = reinterpret_cast<const std::byte*>(text->data());
    auto digest = liquid::hash(liquid::algorithm::sha256, std::span{first, text->size()});
    if (!digest) {
        return fail(digest.error());
    }
    const auto fingerprint = liquid::to_string(*digest);
    std::print("{}", *text);
    std::println("fingerprint={}", fingerprint);
    if (candidate.configuration.witness_threshold == 0) {
        std::println(stderr,
                     "warning: the log asks for no witness cosignatures, so a pin of it gives no "
                     "split-view protection; raise `threshold` in the pin file if witnesses exist");
    }
    if (confirmed.empty()) {
        std::println(stderr,
                     "not pinned: check the key and witnesses with the log's operator, then run "
                     "`liquid log trust {} --confirm {}`",
                     log_url,
                     fingerprint);
        return 0;
    }
    if (confirmed != fingerprint) {
        return fail(liquid::error{liquid::errc::conflict,
                                  "the log's key or witness policy is not the one confirmed; "
                                  "nothing was pinned",
                                  std::string{log_url}});
    }
    auto saved =
        liquid::runtime::block_on(transparency::save_log_trust(std::string{log_url}, candidate));
    if (!saved) {
        return fail(saved.error());
    }
    auto path = transparency::log_trust_path(log_url);
    std::println("pinned {} in {}", log_url, path ? path->string() : std::string{});
    return 0;
}

} // namespace

auto transparency_log(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "timestamps") {
        return repo_log(argc, argv);
    }
    if (argc == 2 && std::string_view{argv[0]} == "verify" &&
        std::string_view{argv[1]} == "--timestamps") {
        return repo_log(argc, argv);
    }
    if (argc == 0 ||
        (argc == 1 &&
         (std::string_view{argv[0]} == "--json" || std::string_view{argv[0]} == "--ops")) ||
        (argc == 2 &&
         ((std::string_view{argv[0]} == "--ops" && std::string_view{argv[1]} == "--json") ||
          (std::string_view{argv[0]} == "--json" && std::string_view{argv[1]} == "--ops")))) {
        return repo_log(argc, argv);
    }
    const auto operation = std::string_view{argv[0]};
    if (operation == "verify" && argc == 3) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto encoded = base64url_decode(argv[2]);
        if (!encoded) {
            return fail(encoded.error());
        }
        auto trust = load_pin(argv[1]);
        if (!trust) {
            return fail(trust.error());
        }
        auto repository = open_repository();
        if (!repository) {
            return fail(repository.error());
        }
        auto client = liquid::transparency::log_client{*source};
        auto verified = liquid::runtime::block_on(
            client.verify_inclusion(*repository, std::move(*encoded), *trust));
        if (!verified) {
            return fail(verified.error());
        }
        print_checkpoint(*verified);
        return 0;
    }
    if (operation == "consistency" && argc == 2) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto trust = load_pin(argv[1]);
        if (!trust) {
            return fail(trust.error());
        }
        auto repository = open_repository();
        if (!repository) {
            return fail(repository.error());
        }
        auto client = liquid::transparency::log_client{*source};
        auto verified = liquid::runtime::block_on(client.verify_consistency(*repository, *trust));
        if (!verified) {
            return fail(verified.error());
        }
        print_checkpoint(*verified);
        return 0;
    }
    if (operation == "scan" && argc == 3) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto trust = load_pin(argv[1]);
        if (!trust) {
            return fail(trust.error());
        }
        auto client = liquid::transparency::log_client{*source};
        auto scan = liquid::runtime::block_on(client.scan(argv[2], *trust));
        if (!scan) {
            return fail(scan.error());
        }
        for (const auto& match : scan->matches) {
            std::println("index={} profile={} issuer={} subject={}",
                         match.index,
                         match.profile,
                         match.issuer,
                         match.subject);
        }
        return 0;
    }
    if (operation == "trust" &&
        (argc == 2 || (argc == 4 && std::string_view{argv[2]} == "--confirm"))) {
        return trust_log(argv[1], argc == 4 ? std::string_view{argv[3]} : std::string_view{});
    }
    return usage();
}

} // namespace liquid_cli
