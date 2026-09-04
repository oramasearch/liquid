#include "liquid/git.hpp"

namespace liquid {

runtime::task<export_report> export_git(repository&, std::filesystem::path, std::string) {
    co_return std::unexpected(
        error{errc::unsupported, "Git export is not implemented", "liquid #43"});
}

} // namespace liquid
