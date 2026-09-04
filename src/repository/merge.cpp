#include "repository/merge.hpp"

namespace liquid::repository_detail {

auto entry_conflict(std::string path, const digest &left, const digest &right)
    -> std::optional<conflict> {
  if (left.alg == right.alg && left.bytes == right.bytes)
    return std::nullopt;
  return conflict{std::move(path), {left, right}};
}

} // namespace liquid::repository_detail
