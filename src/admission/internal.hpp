// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "liquid/admission.hpp"
#include "liquid/transparency.hpp"

namespace liquid::admission_detail {

using checkpoint_clock = std::function<std::chrono::system_clock::time_point()>;

[[nodiscard]] auto open_source(std::string location)
    -> runtime::task<std::shared_ptr<attestation_source>>;
[[nodiscard]] auto make_log_verifier(admission_log_policy policy, std::filesystem::path cache)
    -> std::shared_ptr<log_verifier>;
[[nodiscard]] LIQUID_EXPORT auto
make_log_verifier(admission_log_policy policy,
                  std::filesystem::path cache,
                  std::unique_ptr<transparency::log_source> source,
                  checkpoint_clock clock = std::chrono::system_clock::now)
    -> std::shared_ptr<log_verifier>;

} // namespace liquid::admission_detail
