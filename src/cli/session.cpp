#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(
        stderr,
        "error: usage: session <start <delegation>|write <id> <path> --from <file>|publish "
        "<id> [--message <text>]|discard <id>>");
    return 2;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

auto load_delegation(const liquid::digest& id) -> liquid::result<liquid::attestation> {
    auto objects = liquid::runtime::block_on(
        liquid::store::open(std::filesystem::current_path() / ".liquid-store"));
    if (!objects) {
        return std::unexpected(objects.error());
    }
    auto encoded = liquid::runtime::block_on(objects->get(id));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return liquid::decode_attestation(*encoded);
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read session input", path.string()});
    }
    const auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

auto start(liquid::repository& repository, std::string_view delegation_text) -> int {
    auto id = liquid::parse_digest(delegation_text);
    if (!id) {
        return fail(id.error());
    }
    auto authority = load_delegation(*id);
    if (!authority) {
        return fail(authority.error());
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return fail(signer.error());
    }
    auto created = liquid::runtime::block_on(repository.fork_session(
        {.actor = signer->identity(), .delegation = std::move(*authority)}));
    if (!created) {
        return fail(created.error());
    }
    std::println("{}", liquid::to_string(created->id()));
    return 0;
}

auto write(liquid::repository& repository, int argc, char** argv) -> int {
    if (argc != 4 || std::string_view{argv[2]} != "--from") {
        return usage();
    }
    auto id = liquid::parse_session_id(argv[0]);
    if (!id) {
        return fail(id.error());
    }
    auto contents = read_file(argv[3]);
    if (!contents) {
        return fail(contents.error());
    }
    auto selected = liquid::runtime::block_on(repository.open_session(*id));
    if (!selected) {
        return fail(selected.error());
    }
    auto written = liquid::runtime::block_on(selected->write(argv[1], std::move(*contents)));
    if (!written) {
        return fail(written.error());
    }
    std::println("wrote {} in {}", argv[1], argv[0]);
    return 0;
}

auto publish(liquid::repository& repository, int argc, char** argv) -> int {
    if (argc != 1 && argc != 3) {
        return usage();
    }
    auto options = liquid::publish_options{};
    if (argc == 3) {
        if (std::string_view{argv[1]} != "--message") {
            return usage();
        }
        options.message = argv[2];
    }
    auto id = liquid::parse_session_id(argv[0]);
    if (!id) {
        return fail(id.error());
    }
    auto selected = liquid::runtime::block_on(repository.open_session(*id));
    if (!selected) {
        return fail(selected.error());
    }
    auto report = liquid::runtime::block_on(selected->publish(std::move(options)));
    if (!report) {
        return fail(report.error());
    }
    std::println("published {} paths in {} directories", report->paths.size(), report->directories);
    return 0;
}

auto discard(liquid::repository& repository, std::string_view id_text) -> int {
    auto id = liquid::parse_session_id(id_text);
    if (!id) {
        return fail(id.error());
    }
    auto selected = liquid::runtime::block_on(repository.open_session(*id));
    if (!selected) {
        return fail(selected.error());
    }
    auto discarded = liquid::runtime::block_on(selected->discard());
    if (!discarded) {
        return fail(discarded.error());
    }
    std::println("discarded {}", id_text);
    return 0;
}

} // namespace

auto session(int argc, char** argv) -> int {
    if (argc < 2) {
        return usage();
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    const auto command = std::string_view{argv[0]};
    if (command == "start" && argc == 2) {
        return start(*repository, argv[1]);
    }
    if (command == "write") {
        return write(*repository, argc - 1, argv + 1);
    }
    if (command == "publish") {
        return publish(*repository, argc - 1, argv + 1);
    }
    if (command == "discard" && argc == 2) {
        return discard(*repository, argv[1]);
    }
    return usage();
}

auto compact(int argc, char** argv) -> int {
    if (argc < 1 || std::string_view{argv[0]} != "--epoch") {
        std::println(stderr, "error: usage: compact --epoch [--min-ack-peers <count>]");
        return 2;
    }
    auto options = liquid::epoch_options{};
    if (argc == 3 && std::string_view{argv[1]} == "--min-ack-peers") {
        const auto text = std::string_view{argv[2]};
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), options.min_ack_peers);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return usage();
        }
    } else if (argc != 1) {
        return usage();
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto value = liquid::runtime::block_on(repository->compact(options));
    if (!value) {
        return fail(value.error());
    }
    std::println("epoch={} document_bytes={}", value->n, value->document_bytes);
    return 0;
}

} // namespace liquid_cli
