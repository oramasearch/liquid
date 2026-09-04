#include "repository/merge.hpp"

namespace liquid::repository_detail {

auto entry_conflict(std::string path, const digest &left, const digest &right)
    -> std::optional<conflict> {
  if (left.alg == right.alg && left.bytes == right.bytes)
    return std::nullopt;
  return conflict{std::move(path), {left, right}};
}

auto three_way_entry_conflict(std::string path, const digest &base, const digest &left,
                              const digest &right) -> std::optional<conflict> {
  if ((left.alg == right.alg && left.bytes == right.bytes) ||
      (left.alg == base.alg && left.bytes == base.bytes) ||
      (right.alg == base.alg && right.bytes == base.bytes))
    return std::nullopt;
  return conflict{std::move(path), {base, left, right}};
}

} // namespace liquid::repository_detail
