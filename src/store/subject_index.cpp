// SPDX-License-Identifier: MIT

#include "subject_index.hpp"

#include <algorithm>
#include <ranges>

namespace liquid::store_detail {

void subject_index::clear() {
    entries_.clear();
}

void subject_index::add(digest subject, digest attestation) {
    auto& values = entries_[to_string(subject)];
    const auto duplicate = std::ranges::any_of(values, [&](const digest& value) {
        return value.alg == attestation.alg && value.bytes == attestation.bytes;
    });
    if (!duplicate) {
        values.push_back(attestation);
    }
}

std::vector<digest> subject_index::find(digest subject) const {
    const auto found = entries_.find(to_string(subject));
    return found == entries_.end() ? std::vector<digest>{} : found->second;
}

} // namespace liquid::store_detail
