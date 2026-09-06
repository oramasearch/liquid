#pragma once

#include <memory>
#include <vector>

#include "liquid/resolve.hpp"

namespace liquid::resolve_detail {

class grounded_concretization {
public:
    virtual ~grounded_concretization() = default;
};

class concretizer {
public:
    virtual ~concretizer() = default;

    [[nodiscard]] virtual result<std::unique_ptr<grounded_concretization>>
    ground(package_universe universe, std::vector<spec> roots) const = 0;
    [[nodiscard]] virtual result<resolution> solve(std::unique_ptr<grounded_concretization> problem,
                                                   resolve_options options) const = 0;
};

[[nodiscard]] auto make_concretizer() -> std::shared_ptr<const concretizer>;

} // namespace liquid::resolve_detail
