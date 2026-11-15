#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/release.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: release policy set --threshold <k> --signer <id>... "
                 "[--role <name>] [--not-after <UTC>] | release approve <artifact> | "
                 "release verify <artifact> ()");
    return 2;
}

auto release_root() -> std::filesystem::path {
    return std::filesystem::current_path() / ".liquid" / "release";
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "cannot read release record", path.string()});
    }
    const auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

auto write_file(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> liquid::result<void> {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), path.string()});
    }
    const auto temporary = path.string() + ".tmp";
    auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot write release record", temporary});
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), path.string()});
    }
    return {};
}

auto load_policy() -> liquid::result<liquid::release_policy> {
    auto encoded = read_file(release_root() / "policy.cbor");
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto statement = liquid::decode_attestation(*encoded);
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return liquid::release_policy{std::move(*statement)};
}

auto repository_subject() -> liquid::result<liquid::digest> {
    auto repository = liquid::runtime::block_on(
        liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    auto id = repository->id();
    if (!id) {
        return std::unexpected(id.error());
    }
    return liquid::hash(liquid::algorithm::blake3, *id);
}

auto default_expiry() -> std::string {
    const auto value = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now() + std::chrono::days{90});
    return std::format("{:%FT%TZ}", value);
}

auto parse_threshold(std::string_view input) -> liquid::result<std::uint64_t> {
    auto value = std::uint64_t{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
        value == 0) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "invalid release threshold", std::string{input}});
    }
    return value;
}

auto set_policy(int argc, char** argv) -> int {
    auto threshold = std::uint64_t{};
    auto signers = std::vector<std::string>{};
    auto role = std::string{"targets"};
    auto not_after = default_expiry();
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--threshold" && index + 1 < argc) {
            auto parsed = parse_threshold(argv[++index]);
            if (!parsed) {
                return fail(parsed.error());
            }
            threshold = *parsed;
        } else if (argument == "--signer" && index + 1 < argc) {
            signers.emplace_back(argv[++index]);
        } else if (argument == "--role" && index + 1 < argc) {
            role = argv[++index];
        } else if (argument == "--not-after" && index + 1 < argc) {
            not_after = argv[++index];
        } else {
            return usage();
        }
    }
    if (threshold == 0 || signers.empty()) {
        return usage();
    }
    auto repository = repository_subject();
    if (!repository) {
        return fail(repository.error());
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return fail(signer.error());
    }
    auto policy = liquid::runtime::block_on(liquid::make_release_policy(
        *repository, std::move(role), threshold, std::move(signers), not_after, *signer));
    if (!policy) {
        return fail(policy.error());
    }
    auto encoded = liquid::encode_attestation(policy->statement);
    if (!encoded) {
        return fail(encoded.error());
    }
    auto written = write_file(release_root() / "policy.cbor", *encoded);
    if (!written) {
        return fail(written.error());
    }
    std::println("policy={}", release_root().append("policy.cbor").string());
    std::println("not_after={}", not_after);
    return 0;
}

auto approve(std::string_view artifact_text) -> int {
    auto artifact = liquid::parse_digest(artifact_text);
    if (!artifact) {
        return fail(artifact.error());
    }
    auto policy = load_policy();
    if (!policy) {
        return fail(policy.error());
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return fail(signer.error());
    }
    auto approval = liquid::runtime::block_on(
        liquid::approve_release(*artifact, *policy, std::string{artifact_text}, *signer));
    if (!approval) {
        return fail(approval.error());
    }
    auto encoded = liquid::encode_attestation(approval->statement);
    if (!encoded) {
        return fail(encoded.error());
    }
    const auto identity = signer->identity();
    auto issuer = liquid::hash(liquid::algorithm::blake3,
                               std::as_bytes(std::span{identity.taap_id}));
    if (!issuer) {
        return fail(issuer.error());
    }
    const auto path = release_root() / "approvals" / liquid::to_string(*artifact) /
                      (liquid::to_string(*issuer) + ".cbor");
    auto written = write_file(path, *encoded);
    if (!written) {
        return fail(written.error());
    }
    std::println("approval={}", path.string());
    return 0;
}

auto load_approvals(const liquid::digest& artifact)
    -> liquid::result<std::vector<liquid::release_approval>> {
    const auto directory = release_root() / "approvals" / liquid::to_string(artifact);
    auto output = std::vector<liquid::release_approval>{};
    std::error_code code;
    if (!std::filesystem::exists(directory, code)) {
        return output;
    }
    for (const auto& entry : std::filesystem::directory_iterator{directory, code}) {
        if (code) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".cbor") {
            continue;
        }
        auto encoded = read_file(entry.path());
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto statement = liquid::decode_attestation(*encoded);
        if (!statement) {
            return std::unexpected(statement.error());
        }
        output.push_back({std::move(*statement)});
    }
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), directory.string()});
    }
    return output;
}

// liquid#384: the trust root comes from this machine, never from the repository being released.
auto load_release_trust() -> liquid::result<liquid::release_trust> {
    auto issuers = liquid::load_trusted_identities("release-policy-issuers");
    if (!issuers) {
        return std::unexpected(issuers.error());
    }
    auto repository = repository_subject();
    if (!repository) {
        return std::unexpected(repository.error());
    }
    return liquid::release_trust{.repository = *repository, .policy_issuers = std::move(*issuers)};
}

auto verify(std::string_view artifact_text) -> int {
    auto artifact = liquid::parse_digest(artifact_text);
    if (!artifact) {
        return fail(artifact.error());
    }
    auto trust = load_release_trust();
    if (!trust) {
        return fail(trust.error());
    }
    auto policy = load_policy();
    if (!policy) {
        return fail(policy.error());
    }
    auto approvals = load_approvals(*artifact);
    if (!approvals) {
        return fail(approvals.error());
    }
    auto verified = liquid::verify_release(*artifact, *policy, *approvals, *trust);
    if (!verified) {
        return fail(verified.error());
    }
    std::println("satisfied={}", verified->satisfied);
    std::println("distinct_valid_signers={}", verified->distinct_valid_signers.size());
    for (const auto& missing : verified->missing) {
        std::println("missing={}", missing);
    }
    for (const auto& unknown : verified->unknown_signers) {
        std::println("unknown_signer={}", unknown);
    }
    if (!verified->unknown_signers.empty()) {
        std::println(stderr,
                     "hint: an unknown signer's approval counts only after its identity chain is "
                     "trusted: liquid identity trust <chain>");
    }
    return verified->satisfied ? 0 : 1;
}

} // namespace

auto release(int argc, char** argv) -> int {
    if (argc >= 2 && std::string_view{argv[0]} == "policy" &&
        std::string_view{argv[1]} == "set") {
        return set_policy(argc - 2, argv + 2);
    }
    if (argc == 2 && std::string_view{argv[0]} == "approve") {
        return approve(argv[1]);
    }
    if (argc == 2 && std::string_view{argv[0]} == "verify") {
        return verify(argv[1]);
    }
    return usage();
}

} // namespace liquid_cli
