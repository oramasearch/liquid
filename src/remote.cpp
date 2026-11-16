// SPDX-License-Identifier: MIT

#include "liquid/remote.hpp"

#include <utility>

#include "liquid/build.hpp"
#include "reapi/remote_client.hpp"

namespace liquid {

runtime::task<void> remote_blob_source::prepare(std::string, std::vector<remote_blob>) {
    co_return result<void>{};
}

runtime::task<action_result>
remote_executor::execute_prepared(action_input input, digest, digest, execution_schedule) {
    co_return co_await execute(std::move(input));
}

struct remote::impl {
    explicit impl(std::shared_ptr<reapi::remote_client> client) : client(std::move(client)) {}

    std::shared_ptr<reapi::remote_client> client;
};

remote::remote() noexcept = default;
remote::~remote() = default;
remote::remote(remote&&) noexcept = default;
remote& remote::operator=(remote&&) noexcept = default;

runtime::task<remote> remote::connect(remote_config config) {
    if (config.endpoint.empty()) {
        co_return std::unexpected(
            error{errc::invalid_argument, "remote endpoint is empty", "remote::connect"});
    }
    if (config.deadline <= std::chrono::milliseconds{}) {
        co_return std::unexpected(
            error{errc::invalid_argument, "remote deadline must be positive", "remote::connect"});
    }
    auto client = std::make_shared<reapi::remote_client>(std::move(config));
    auto initialized = co_await client->initialize();
    if (!initialized) {
        co_return std::unexpected(initialized.error());
    }
    auto output = remote{};
    output.impl_ = std::make_unique<impl>(std::move(client));
    co_return output;
}

remote_blob_source& remote::blobs() noexcept {
    return *impl_->client;
}

remote_executor& remote::executor() noexcept {
    return *impl_->client;
}

const remote_config& remote::config() const noexcept {
    return impl_->client->config();
}

algorithm remote::digest_algorithm() const noexcept {
    return impl_->client->selected_algorithm();
}

runtime::task<std::shared_ptr<action_result>>
remote::get_action_result(repository& source, action_input input, digest sandbox) {
    auto value = co_await impl_->client->get_action_result(source, std::move(input), sandbox);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (!*value) {
        co_return std::shared_ptr<action_result>{};
    }
    co_return std::make_shared<action_result>(std::move(**value));
}

runtime::task<void> remote::update_action_result(repository& source,
                                                 action_input input,
                                                 digest sandbox,
                                                 const action_result& value) {
    co_return co_await impl_->client->update_action_result(
        source, std::move(input), sandbox, value);
}

} // namespace liquid
