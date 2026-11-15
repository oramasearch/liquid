#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "admission/checkpoint_fixture.hpp"
#include "cli/verify.hpp"
#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"
#include "objects/records.hpp"

namespace {

using json = nlohmann::json;

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto hex(std::span<const std::byte> value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto number = std::to_integer<unsigned char>(byte);
        output.push_back(digits[number >> 4U]);
        output.push_back(digits[number & 15U]);
    }
    return output;
}

auto entry_tile(std::span<const std::byte> entry) -> std::vector<std::byte> {
    auto output = bytes("LAKE-TILE-1\n");
    const auto length = static_cast<std::uint32_t>(entry.size());
    output.push_back(std::byte{static_cast<unsigned char>(length >> 24U)});
    output.push_back(std::byte{static_cast<unsigned char>(length >> 16U)});
    output.push_back(std::byte{static_cast<unsigned char>(length >> 8U)});
    output.push_back(std::byte{static_cast<unsigned char>(length)});
    output.insert(output.end(), entry.begin(), entry.end());
    return output;
}

auto signed_reproducibility(liquid::digest subject, liquid::signer& signer) -> liquid::attestation {
    auto value = liquid::attestation{
        .subject = subject,
        .profile = "reproducibility/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .issuer = signer.identity(),
        .subject_kind = liquid::attestation_subject_kind::artifact,
        .attributes = {{"verified_by", signer.identity().taap_id},
                       {"sandbox", "blake3:verify-log-cli"},
                       {"agreement", true}}};
    auto body = liquid::objects::encode_attestation_body(value);
    if (!body) {
        throw std::runtime_error(body.error().message);
    }
    auto signature = liquid::runtime::block_on(signer.sign("attestation", *body));
    if (!signature) {
        throw std::runtime_error(signature.error().message);
    }
    auto encoded_signature = liquid::encode_attestation_signature(*signature);
    if (!encoded_signature) {
        throw std::runtime_error(encoded_signature.error().message);
    }
    value.signature = std::move(*encoded_signature);
    return value;
}

struct log_fixture {
    liquid::transparency::log_trust trust;
    json identity;
    json configuration;
    json checkpoint;
    json tile;
    json proof;
};

auto make_log_fixture(std::span<const std::byte> entry, bool tampered) -> log_fixture {
    auto leaf_input = std::vector<std::byte>{std::byte{}};
    leaf_input.insert(leaf_input.end(), entry.begin(), entry.end());
    auto root = liquid::hash(liquid::algorithm::sha256, leaf_input);
    if (!root) {
        throw std::runtime_error(root.error().message);
    }
    const auto body = json{{"version", "lake_checkpoint/1"},
                           {"hash_algorithm", "sha256"},
                           {"tree_size", 1},
                           {"tile_width", 1024},
                           {"hash_tile_width", 1024},
                           {"root_hash", hex(root->bytes)}};
    const auto checkpoint = liquid::test::admission::sign_checkpoint(body);
    const auto tile_bytes = entry_tile(entry);
    const auto tile_hash = liquid::hash(liquid::algorithm::sha256, tile_bytes);
    if (!tile_hash) {
        throw std::runtime_error(tile_hash.error().message);
    }
    auto proof = json{{"version", "lake_inclusion_proof/1"},
                      {"hash_algorithm", "sha256"},
                      {"leaf_index", 0},
                      {"tree_size", 1},
                      {"root_hash", hex(root->bytes)},
                      {"checkpoint", checkpoint.record},
                      {"audit_path", json::array()}};
    if (tampered) {
        proof["audit_path"].push_back(std::string(64, '0'));
    }
    return {
        .trust = checkpoint.trust,
        .identity = {{"taap_id", checkpoint.trust.identity.taap_id},
                     {"public_key",
                      liquid::test::admission::base64url(checkpoint.trust.identity.public_key)}},
        .configuration = {{"witness_threshold", 0}, {"witnesses", json::array()}},
        .checkpoint = checkpoint.record,
        .tile = {{"version", "lake_tile/1"},
                 {"kind", "entries"},
                 {"encoding", "base64url"},
                 {"level", 0},
                 {"index", 0},
                 {"entries", 1},
                 {"tile", liquid::test::admission::base64url(tile_bytes)},
                 {"sha256", hex(tile_hash->bytes)}},
        .proof = std::move(proof)};
}

class local_log final {
public:
    explicit local_log(log_fixture fixture) : fixture_(std::move(fixture)) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) {
            throw std::runtime_error("could not create loopback log socket");
        }
        int enabled = 1;
        if (::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
            throw std::runtime_error("could not configure loopback log socket");
        }
        auto address = sockaddr_in{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(socket_, 8) != 0) {
            throw std::runtime_error("could not start loopback log socket");
        }
        auto length = static_cast<socklen_t>(sizeof(address));
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            throw std::runtime_error("could not read loopback log port");
        }
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        worker_ = std::thread([this] { serve(); });
    }

    ~local_log() {
        stopping_ = true;
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        worker_.join();
    }

    local_log(const local_log&) = delete;
    auto operator=(const local_log&) -> local_log& = delete;

    [[nodiscard]] auto url() const -> const std::string& {
        return url_;
    }

private:
    auto response(std::string_view target) const -> std::string {
        const json* value = nullptr;
        if (target.starts_with("/v1/identity")) {
            value = &fixture_.identity;
        } else if (target.starts_with("/v1/transparency_log_configuration")) {
            value = &fixture_.configuration;
        } else if (target.starts_with("/v1/transparency_checkpoint")) {
            value = &fixture_.checkpoint;
        } else if (target.starts_with("/v1/transparency_tile")) {
            value = &fixture_.tile;
        } else if (target.starts_with("/v1/transparency_inclusion_proof")) {
            value = &fixture_.proof;
        }
        const auto body = value ? value->dump() : json{{"ok", false}}.dump();
        return "HTTP/1.1 " + std::string{value ? "200 OK" : "404 Not Found"} +
               "\r\nContent-Type: application/json\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    }

    void serve() {
        while (!stopping_) {
            auto readable = fd_set{};
            FD_ZERO(&readable);
            FD_SET(socket_, &readable);
            auto timeout = timeval{.tv_sec = 0, .tv_usec = 100000};
            if (::select(socket_ + 1, &readable, nullptr, nullptr, &timeout) <= 0) {
                continue;
            }
            const auto client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            auto request = std::array<char, 2048>{};
            const auto read = ::recv(client, request.data(), request.size() - 1, 0);
            const auto first =
                std::string_view{request.data(), read > 0 ? static_cast<std::size_t>(read) : 0};
            const auto start = first.find(' ');
            const auto end = start == std::string_view::npos ? start : first.find(' ', start + 1);
            const auto target = end == std::string_view::npos
                                    ? std::string_view{}
                                    : first.substr(start + 1, end - start - 1);
            const auto wire = response(target);
            static_cast<void>(::send(client, wire.data(), wire.size(), 0));
            ::close(client);
        }
    }

    log_fixture fixture_;
    int socket_{};
    std::atomic_bool stopping_{};
    std::string url_;
    std::thread worker_;
};

class current_directory final {
public:
    explicit current_directory(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~current_directory() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

auto invoke(const std::string& url, liquid::digest attestation, bool require_log = true) -> int {
    auto all = std::string{"--all"};
    auto log = std::string{"--log"};
    auto mutable_url = url;
    auto arguments = std::array<char*, 3>{all.data(), log.data(), mutable_url.data()};
    return liquid_cli::verify_detail::run(
        require_log ? static_cast<int>(arguments.size()) : 1,
        arguments.data(),
        [attestation](std::filesystem::path, liquid::verify_options options) {
            if (options.selection != liquid::verify_selection::all) {
                return liquid::result<liquid::verify_report>{std::unexpected(liquid::error{
                    liquid::errc::invalid_argument, "verify-log fixture expected --all", {}})};
            }
            const auto report = liquid::verify_report{
                .considered = 1, .actions = {{attestation, attestation, attestation, true, {}}}};
            return liquid::result<liquid::verify_report>{report};
        });
}

// Writes the operator's pin for `url` into $LIQUID_HOME/trust/transparency/ (liquid#389).
// A port the kernel hands out again would find an earlier case's pin, so that pin is removed.
void pin(const std::string& url, const liquid::transparency::log_trust& trust) {
    auto path = liquid::transparency::log_trust_path(url);
    if (!path) {
        throw std::runtime_error(path.error().message);
    }
    std::error_code code;
    std::filesystem::remove(*path, code);
    auto saved = liquid::runtime::block_on(liquid::transparency::save_log_trust(url, trust));
    if (!saved) {
        throw std::runtime_error(saved.error().message);
    }
}

} // namespace

auto main() -> int {
    try {
        const auto root = std::filesystem::temp_directory_path() /
                          ("liquid-verify-log-" + std::to_string(::getpid()));
        std::error_code code;
        std::filesystem::remove_all(root, code);
        std::filesystem::create_directories(root / "identity", code);
        if (code) {
            throw std::runtime_error(code.message());
        }
        setenv("LIQUID_HOME", (root / "identity").c_str(), 1);
        auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
        if (!signer) {
            throw std::runtime_error(signer.error().message);
        }
        auto repository =
            liquid::runtime::block_on(liquid::repository::create(root / "repository"));
        if (!repository) {
            throw std::runtime_error(repository.error().message);
        }
        const auto selected =
            signed_reproducibility(liquid::digest{liquid::algorithm::blake3, {}}, *signer);
        const auto missing =
            signed_reproducibility(liquid::digest{liquid::algorithm::sha256, {}}, *signer);
        auto selected_id = liquid::runtime::block_on(repository->store_attestation(selected));
        auto missing_id = liquid::runtime::block_on(repository->store_attestation(missing));
        if (!selected_id || !missing_id) {
            throw std::runtime_error(!selected_id ? selected_id.error().message
                                                  : missing_id.error().message);
        }
        auto selected_bytes = liquid::encode_attestation(selected);
        auto missing_bytes = liquid::encode_attestation(missing);
        if (!selected_bytes || !missing_bytes) {
            throw std::runtime_error("could not encode reproducibility attestation");
        }
        {
            auto directory = current_directory{root / "repository"};
            if (invoke({}, *selected_id, false) != 0) {
                throw std::runtime_error("local-only verification failed");
            }
            {
                auto fixture = make_log_fixture(*selected_bytes, false);
                const auto trust = fixture.trust;
                auto server = local_log{std::move(fixture)};
                if (invoke(server.url(), *selected_id) != 1) {
                    throw std::runtime_error("an unpinned log was trusted");
                }
                pin(server.url(), trust);
                if (invoke(server.url(), *selected_id) != 0) {
                    throw std::runtime_error("valid required inclusion failed");
                }
            }
            {
                // The log reports no witnesses and threshold 0; the pin requires one witness.
                auto fixture = make_log_fixture(*selected_bytes, false);
                auto trust = fixture.trust;
                trust.configuration.witnesses = {{"https://witness.example/checkpoint",
                                                  liquid::transparency::hash{std::byte{1}}}};
                trust.configuration.witness_threshold = 1;
                auto server = local_log{std::move(fixture)};
                pin(server.url(), trust);
                if (invoke(server.url(), *selected_id) != 1) {
                    throw std::runtime_error("the log's lower witness threshold was trusted");
                }
            }
            {
                auto fixture = make_log_fixture(*missing_bytes, false);
                const auto trust = fixture.trust;
                auto server = local_log{std::move(fixture)};
                pin(server.url(), trust);
                if (invoke(server.url(), *selected_id) != 1) {
                    throw std::runtime_error("missing required inclusion did not fail");
                }
            }
            {
                auto fixture = make_log_fixture(*selected_bytes, true);
                const auto trust = fixture.trust;
                auto server = local_log{std::move(fixture)};
                pin(server.url(), trust);
                if (invoke(server.url(), *selected_id) != 1) {
                    throw std::runtime_error("tampered required inclusion did not fail");
                }
            }
            pin("http://127.0.0.1:1", make_log_fixture(*selected_bytes, false).trust);
            if (invoke("http://127.0.0.1:1", *selected_id) != 1) {
                throw std::runtime_error("unavailable required inclusion did not fail");
            }
        }
        std::filesystem::remove_all(root, code);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "verify-log fixture failed: " << error.what() << '\n';
        return 1;
    }
}
