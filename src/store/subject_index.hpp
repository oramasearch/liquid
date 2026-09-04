#pragma once

#include <map>
#include <vector>

#include "liquid/hash.hpp"

namespace liquid::store_detail {

class subject_index {
public:
    void clear();
    void add(digest subject, digest attestation);
    [[nodiscard]] std::vector<digest> find(digest subject) const;

private:
    std::map<std::string, std::vector<digest>> entries_;
};

} // namespace liquid::store_detail
