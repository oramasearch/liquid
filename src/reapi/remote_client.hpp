#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "liquid/remote.hpp"
#include "reapi/client.hpp"
#include "reapi/messages.hpp"

namespace liquid::reapi {

class remote_client final : public remote_blob_source, public remote_executor {
public:
    explicit remote_client(remote_config config);

    [[nodiscard]] runtime::task<void> initialize();
    [[nodiscard]] runtime::task<void> prepare(std::string instance,
                                              std::vector<remote_blob> blobs) override;
    [[nodiscard]] runtime::task<std::vector<digest>>
    find_missing(std::string instance, std::vector<digest> blobs) override;
    [[nodiscard]] runtime::task<std::uint64_t> read_range(std::string instance,
                                                          digest blob,
                                                          std::uint64_t offset,
                                                          std::span<std::byte> output) override;
    [[nodiscard]] runtime::task<void>
    write(std::string instance, digest blob, std::vector<std::byte> contents) override;

    [[nodiscard]] remote_blob_source& blobs() noexcept override;
    [[nodiscard]] std::string instance() const override;
    [[nodiscard]] std::uint64_t queue_depth() const noexcept override;
    [[nodiscard]] bool supports_batch() const noexcept override;
    [[nodiscard]] runtime::task<action_result> execute(action_input input) override;
    [[nodiscard]] runtime::task<action_result> execute_prepared(
        action_input input, digest action, digest sandbox, execution_schedule schedule) override;
    [[nodiscard]] runtime::task<std::vector<action_result>>
    execute_batch(std::vector<action_input> inputs) override;

    [[nodiscard]] const remote_config& config() const noexcept;
    [[nodiscard]] algorithm selected_algorithm() const noexcept;
    [[nodiscard]] runtime::task<std::optional<action_result>>
    get_action_result(repository& source, action_input input, digest sandbox);
    [[nodiscard]] runtime::task<void> update_action_result(repository& source,
                                                           action_input input,
                                                           digest sandbox,
                                                           const action_result& value);

private:
    struct prepared_reapi_action;

    grpc_client rpc_;
    algorithm selected_{algorithm::sha256};
    std::map<std::string, std::vector<std::byte>> staged_;
    std::map<std::string, Digest> aliases_;

    [[nodiscard]] runtime::task<prepared_reapi_action> prepare_action(const action_input& input,
                                                                      digest sandbox);
    [[nodiscard]] runtime::task<void> upload_prepared(const prepared_reapi_action& prepared);
    [[nodiscard]] runtime::task<std::vector<std::byte>> read_blob(const Digest& blob);
    [[nodiscard]] runtime::task<action_result>
    native_result(const ActionResult& value, digest action, bool validate_action = true);
    [[nodiscard]] runtime::task<action_result>
    execute_operation(action_input input,
                      digest action,
                      digest sandbox,
                      std::optional<execution_schedule> schedule);
};

} // namespace liquid::reapi
