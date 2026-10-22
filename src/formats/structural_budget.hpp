#pragma once

#include <cstddef>

namespace liquid::formats {

// Structural parsing is bounded independently of archive nesting and decoded byte limits.
// One budget belongs to a complete document; nested containers never reset it.
class structural_budget {
public:
    static constexpr auto maximum_depth = std::size_t{64};
    static constexpr auto maximum_work = std::size_t{1'000'000};

    [[nodiscard]] auto consume(std::size_t depth, std::size_t work = 1) -> bool {
        if (depth >= maximum_depth || work > remaining_) {
            return false;
        }
        remaining_ -= work;
        return true;
    }

private:
    std::size_t remaining_{maximum_work};
};

} // namespace liquid::formats
