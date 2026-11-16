// Issues a `delegation/1` for `liquid session start` tests: run in a repository with the CLI's
// LIQUID_HOME, it asks the repository to sign a delegation to the agent named on the command line
// (`repository::issue_delegation`, liquid#538) and prints its digest. It uses only the public
// libliquid API.
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

auto issue(std::string_view agent_id) -> liquid::result<liquid::digest> {
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    auto issued = liquid::runtime::block_on(
        repository->issue_delegation({.agent = liquid::actor{.taap_id = std::string{agent_id}},
                                      .scopes = {"**"},
                                      .may_publish = true,
                                      .may_lease = true,
                                      .lifetime = std::chrono::hours{1}}));
    if (!issued) {
        return std::unexpected(issued.error());
    }
    return issued->id;
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
