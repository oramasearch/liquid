// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/identity.hpp"

namespace liquid::custody_detail {

class tpm_transport {
public:
    virtual ~tpm_transport() = default;
    [[nodiscard]] virtual result<std::vector<std::byte>>
    submit(std::span<const std::byte> command) = 0;
};

[[nodiscard]] auto make_tpm_provider(std::string parameters)
    -> result<std::shared_ptr<key_provider>>;
LIQUID_EXPORT void install_tpm_transport(std::shared_ptr<tpm_transport> transport);

} // namespace liquid::custody_detail
