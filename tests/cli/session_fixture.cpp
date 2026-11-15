// Issues a `delegation/1` for `liquid session start` tests: run in a repository with the CLI's
// LIQUID_HOME, it signs a delegation to the agent named on the command line with this machine's
// identity, bound to the repository, stores it in `.liquid-store` and prints its digest.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"

namespace {

auto repository_text(const liquid::node_id& id) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto octet : id) {
        const auto value = std::to_integer<unsigned char>(octet);
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0f]);
    }
    return output;
}

auto issue(std::string_view agent_id) -> liquid::result<liquid::digest> {
    const auto root = std::filesystem::current_path();
    auto id = [&]() -> liquid::result<liquid::node_id> {
        auto repository = liquid::runtime::block_on(liquid::repository::open(root));
        if (!repository) {
            return std::unexpected(repository.error());
        }
        return repository->id();
    }();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return std::unexpected(signer.error());
    }
    auto subject = liquid::delegation_subject(liquid::actor{.taap_id = std::string{agent_id}});
    if (!subject) {
        return std::unexpected(subject.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto not_after =
        static_cast<std::uint64_t>((now + std::chrono::hours{1}).time_since_epoch().count());
    auto value = liquid::attestation{.subject = *subject,
                                     .profile = "delegation/1",
                                     .at = now,
                                     .issuer = signer->identity(),
                                     .subject_kind = liquid::attestation_subject_kind::actor,
                                     .attributes = {{"scopes", std::vector<std::string>{"**"}},
                                                    {"may_publish", true},
                                                    {"may_lease", true},
                                                    {"depth_remaining", std::uint64_t{0}},
                                                    {"dispatcher", signer->identity().taap_id},
                                                    {"not_after", not_after},
                                                    {"repository", repository_text(*id)}}};
    auto body = liquid::objects::encode_attestation_body(value);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    auto signature = liquid::encode_attestation_signature(*signed_value);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    value.signature = std::move(*signature);
    auto encoded = liquid::encode_attestation(value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto objects = liquid::runtime::block_on(liquid::store::open(root / ".liquid-store"));
    if (!objects) {
        return std::unexpected(objects.error());
    }
    return liquid::runtime::block_on(objects->put_plain(std::move(*encoded)));
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        std::cerr << "usage: liquid-session-cli-fixture <agent-taap-id>\n";
        return 2;
    }
    auto issued = issue(argv[1]);
    if (!issued) {
        std::cerr << issued.error().message << " (" << issued.error().context << ")\n";
        return 1;
    }
    std::cout << liquid::to_string(*issued) << '\n';
    return 0;
}
