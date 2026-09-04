#pragma once

#include "collector.hpp"

namespace liquid::store_detail {

class store_backend;

class compactor {
public:
    explicit compactor(store_backend& store) : store_(store) {}

    [[nodiscard]] runtime::task<void>
    run(collection_options options, collection_manifest manifest, collection_report& report);

private:
    [[nodiscard]] runtime::task<void>
    compact(std::uint32_t old_pack_id, collection_manifest& manifest, collection_report& report);
    void describe(const collection_manifest& manifest, collection_report& report) const;

    store_backend& store_;
};

} // namespace liquid::store_detail
