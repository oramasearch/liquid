#include "liquid/git.hpp"

namespace liquid {

runtime::task<import_report> import_git(repository&, std::filesystem::path) {
    co_return std::unexpected(
        error{errc::unsupported, "Git import is not implemented", "liquid #43"});
}

} // namespace liquid
