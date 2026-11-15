// SPDX-License-Identifier: MIT

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
               std::chrono::sys_seconds now = {});

} // namespace liquid
