#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT release_policy {
    attestation statement;
};

struct LIQUID_EXPORT release_approval {
    attestation statement;
};

struct LIQUID_EXPORT release_verification {
    bool satisfied{};
    std::vector<std::string> distinct_valid_signers;
    std::vector<std::string> missing;
    // Listed signers whose approval is present but whose identity chain this machine does not
    // hold. Their approvals do not count until the chain is trusted (`trust_identity_chain`).
    std::vector<std::string> unknown_signers;
};

// The trust root for release approval (liquid#384). It is held outside the repository: the policy
// file travels with the repository, so whoever can write the repository could otherwise replace
// it with one they signed themselves. The CLI reads `policy_issuers` from
// `$LIQUID_HOME/trust/release-policy-issuers`.
struct LIQUID_EXPORT release_trust {
    // The repository being released. A policy whose subject is another repository is refused.
    digest repository{};
    // TAAP identity ids allowed to issue the release policy. Each issuer's signature is verified
    // through its identity chain in this machine's identity store. Empty refuses every policy.
    std::vector<std::string> policy_issuers;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<release_policy>
make_release_policy(digest repository,
                    std::string role,
                    std::uint64_t threshold,
                    std::vector<std::string> signers,
                    std::string not_after,
                    signer& policy_signer);

[[nodiscard]] LIQUID_EXPORT runtime::task<release_approval>
approve_release(digest artifact,
                const release_policy& policy,
                std::string release,
                signer& approving_signer);

[[nodiscard]] LIQUID_EXPORT result<release_verification>
verify_release(digest artifact,
               const release_policy& policy,
               std::span<const release_approval> approvals,
               const release_trust& trust,
               std::chrono::sys_seconds now = {});

} // namespace liquid
