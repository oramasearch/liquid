// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "liquid/export.hpp"

namespace liquid {

struct LIQUID_EXPORT token_bucket_config {
    std::uint64_t rate_per_s{10'000};
    std::uint64_t burst{10'000};

    auto operator==(const token_bucket_config&) const -> bool = default;
};

struct LIQUID_EXPORT flow_config {
    token_bucket_config per_actor{};
    token_bucket_config per_document{};
    token_bucket_config per_repository{};
    std::map<std::string, token_bucket_config> actor_overrides;
    std::uint64_t max_update_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t max_unacknowledged{4'096};
    std::uint64_t pause_above{2'048};
    std::uint64_t resume_below{1'024};

    auto operator==(const flow_config&) const -> bool = default;
};

enum class flow_scope : std::uint8_t { actor = 1, document = 2, repository = 3 };

struct LIQUID_EXPORT flow_bucket_status {
    flow_scope scope{flow_scope::actor};
    std::string key;
    double tokens{};
    std::uint64_t rate{};
    std::uint64_t burst{};
    std::uint64_t rejected{};
};

enum class flow_pause_reason : std::uint8_t { none = 0, persistence = 1, consumers = 2 };

struct LIQUID_EXPORT flow_status {
    bool paused{};
    flow_pause_reason reason{flow_pause_reason::none};
    std::uint64_t persistence_lag{};
    std::uint64_t consumer_lag{};
    std::vector<flow_bucket_status> buckets;
};

} // namespace liquid
