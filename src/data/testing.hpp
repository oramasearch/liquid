#pragma once

#include <filesystem>

#include "liquid/data.hpp"

namespace liquid::data_detail::testing {

[[nodiscard]] runtime::task<data_branch_mechanism> select_mechanism(
    data_branch_mechanism requested, std::filesystem::path destination, bool local_quiescent);

[[nodiscard]] runtime::task<std::string>
current_database(repository& source, session_id session, std::string url_environment);

} // namespace liquid::data_detail::testing
