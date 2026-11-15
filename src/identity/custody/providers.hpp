#pragma once

#include <memory>
#include <string>

#include "liquid/identity.hpp"

namespace liquid::custody_detail {

[[nodiscard]] auto make_age_provider(std::string recipients)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_aws_kms_provider(std::string resource)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_gcp_kms_provider(std::string resource)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_azure_kv_provider(std::string resource)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_lake_provider(std::string resource)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_shamir_provider(std::string parameters)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_tpm_provider(std::string parameters)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_keychain_provider(std::string account)
    -> result<std::shared_ptr<key_provider>>;
[[nodiscard]] auto make_shamir_split(std::uint8_t threshold, std::uint8_t shares)
    -> result<custody_split>;

} // namespace liquid::custody_detail
