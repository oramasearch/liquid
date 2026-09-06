#pragma once

#include <optional>
#include <string>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT deploy_request {
    std::string environment;
    std::optional<change_id> change;
    std::string image;
    std::optional<result_id> result;
    std::vector<std::string> secret_references;
};

struct LIQUID_EXPORT deployment_record {
    std::string id;
    std::string repository;
    std::string environment;
    change_id change;
    std::string image;
    std::optional<result_id> result;
    std::optional<std::string> rollback_of;
};

struct LIQUID_EXPORT environment_status {
    std::string phase;
    std::string url;
    bool ready{};
    std::string message;
};

struct LIQUID_EXPORT deploy_options {
    std::string bearer_token;
};

namespace deploy {

[[nodiscard]] LIQUID_EXPORT runtime::task<deployment_record> create(repository& source,
                                                                    std::string control_plane_url,
                                                                    deploy_request request,
                                                                    deploy_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<deployment_record>>
list(repository& source, std::string control_plane_url, deploy_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<deployment_record>
rollback(repository& source,
         std::string control_plane_url,
         std::string environment,
         std::string deployment_or_change,
         deploy_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<environment_status> status(repository& source,
                                                                     std::string control_plane_url,
                                                                     std::string environment,
                                                                     deploy_options options = {});

} // namespace deploy
} // namespace liquid
