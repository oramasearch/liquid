#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "liquid/attestation.hpp"
#include "liquid/export.hpp"
#include "liquid/node_store.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"

namespace liquid::serve {
class route_table;
}

namespace liquid::node_store_detail {

class LIQUID_EXPORT service : public std::enable_shared_from_this<service> {
public:
    [[nodiscard]] static runtime::task<std::shared_ptr<service>> open(node_store_options options);

    [[nodiscard]] runtime::task<mount_result> mount_image(mount_request request);
    [[nodiscard]] runtime::task<void> unmount_image(unmount_request request);
    [[nodiscard]] runtime::task<node_store_status> prefetch(prefetch_request request);
    [[nodiscard]] runtime::task<node_store_status> status();
    [[nodiscard]] runtime::task<void> evict(digest content);
    [[nodiscard]] runtime::task<void> set_budget(std::uint64_t bytes);
    [[nodiscard]] runtime::task<std::string> attestations(digest subject);
    [[nodiscard]] runtime::task<digest> store_local_attestation(attestation value);
    [[nodiscard]] runtime::task<void> register_source(std::string id,
                                                      std::shared_ptr<remote_blob_source> source);

    [[nodiscard]] std::shared_ptr<const serve::route_table> unix_routes();
    [[nodiscard]] std::shared_ptr<const serve::route_table> control_routes();
    [[nodiscard]] const node_store_options& options() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    void socket_bound() noexcept;
    void note_error(const error& value);

private:
    struct impl;

    explicit service(node_store_options options);
    std::unique_ptr<impl> impl_;
};

} // namespace liquid::node_store_detail
