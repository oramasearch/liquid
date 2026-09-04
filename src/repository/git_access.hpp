#pragma once

#include <vector>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

class git_batch {
public:
    git_batch() = default;
    ~git_batch();
    git_batch(git_batch&& source) noexcept;
    auto operator=(git_batch&& source) noexcept -> git_batch&;
    git_batch(const git_batch&) = delete;
    auto operator=(const git_batch&) -> git_batch& = delete;

    [[nodiscard]] runtime::task<void> commit();

private:
    friend struct git_access;

    repository* destination_{};
    bool active_{};
};

struct git_access {
    [[nodiscard]] static git_batch batch(repository& destination);
    static void abort_batch(repository& destination);
    [[nodiscard]] static runtime::task<void> commit_batch(repository& destination);
    [[nodiscard]] static runtime::task<digest> put_blob(repository& destination,
                                                        std::vector<std::byte> contents);
    [[nodiscard]] static runtime::task<digest> put_tree(repository& destination, tree value);
};

} // namespace liquid::repository_detail
