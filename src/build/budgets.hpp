// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

namespace liquid::build_detail {

constexpr auto repository_journal_byte_budget = std::size_t{64 * 1024};
constexpr auto restored_output_buffer_bytes = std::size_t{64 * 1024};
constexpr auto restored_output_files_in_flight = std::size_t{1};

} // namespace liquid::build_detail
