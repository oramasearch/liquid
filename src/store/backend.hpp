// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "liquid/store.hpp"

namespace liquid::store_detail {

[[nodiscard]] LIQUID_EXPORT std::optional<std::filesystem::path>
backend_local_path(const std::shared_ptr<::liquid::store_backend>& backend, std::string_view key);
// How many mutations (put, put_if_absent, put_range, remove) the disk backend under `backend` has
// performed since it was made; zero for any other backend. A store compares two readings to learn
// whether one write-lock hold changed anything (liquid #526).
[[nodiscard]] LIQUID_EXPORT std::uint64_t
backend_mutations(const std::shared_ptr<::liquid::store_backend>& backend) noexcept;
[[nodiscard]] LIQUID_EXPORT runtime::task<bool>
backend_exists(const std::shared_ptr<::liquid::store_backend>& backend, std::string key);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::uint64_t>
backend_size(const std::shared_ptr<::liquid::store_backend>& backend, std::string key);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<std::byte>>
read_backend_object(const std::shared_ptr<::liquid::store_backend>& backend, std::string key);
[[nodiscard]] LIQUID_EXPORT runtime::task<void>
remove_backend_prefix(const std::shared_ptr<::liquid::store_backend>& backend, std::string prefix);

} // namespace liquid::store_detail
