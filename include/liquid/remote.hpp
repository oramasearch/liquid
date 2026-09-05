#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct action_input;
struct action_result;
struct execution_schedule;
class repository;

enum class remote_backend : std::uint8_t { reapi, liquid_scheduler };

struct LIQUID_EXPORT remote_tls_config {
    bool enabled{};
    bool verify_peer{true};
    std::optional<std::filesystem::path> certificate_authority;
    std::optional<std::filesystem::path> client_certificate;
    std::optional<std::filesystem::path> client_key;

    auto operator==(const remote_tls_config&) const -> bool = default;
};

struct LIQUID_EXPORT remote_config {
    std::string endpoint;
    std::string instance_name;
    remote_tls_config tls;
    std::map<std::string, std::string> headers;
    std::chrono::milliseconds deadline{std::chrono::seconds{60}};
    std::uint64_t batch_blob_limit{4U * 1024U * 1024U};
    bool publish{};
    remote_backend backend{remote_backend::reapi};

    auto operator==(const remote_config&) const -> bool = default;
};

struct LIQUID_EXPORT remote_blob_location {
    std::string source;
    std::string instance;

    auto operator==(const remote_blob_location&) const -> bool = default;
};

struct LIQUID_EXPORT remote_blob {
    digest id;
    std::vector<std::byte> contents;
};

class LIQUID_EXPORT remote_blob_source {
public:
    virtual ~remote_blob_source() = default;

    [[nodiscard]] virtual runtime::task<void> prepare(std::string instance,
                                                      std::vector<remote_blob> blobs);
    [[nodiscard]] virtual runtime::task<std::vector<digest>>
    find_missing(std::string instance, std::vector<digest> blobs) = 0;
    [[nodiscard]] virtual runtime::task<std::uint64_t> read_range(std::string instance,
                                                                  digest blob,
                                                                  std::uint64_t offset,
                                                                  std::span<std::byte> output) = 0;
    [[nodiscard]] virtual runtime::task<void>
    write(std::string instance, digest blob, std::vector<std::byte> contents) = 0;
};

class LIQUID_EXPORT remote_executor {
public:
    virtual ~remote_executor() = default;

    [[nodiscard]] virtual remote_blob_source& blobs() noexcept = 0;
    [[nodiscard]] virtual std::string instance() const = 0;
    [[nodiscard]] virtual std::uint64_t queue_depth() const noexcept = 0;
    [[nodiscard]] virtual bool supports_batch() const noexcept = 0;
    [[nodiscard]] virtual runtime::task<action_result> execute(action_input input) = 0;
    [[nodiscard]] virtual runtime::task<std::vector<action_result>>
    execute_batch(std::vector<action_input> inputs) = 0;
    [[nodiscard]] virtual runtime::task<action_result> execute_prepared(
        action_input input, digest action, digest sandbox, execution_schedule schedule);
};

class LIQUID_EXPORT remote {
public:
    remote() noexcept;
    ~remote();
    remote(remote&&) noexcept;
    remote& operator=(remote&&) noexcept;
    remote(const remote&) = delete;
    remote& operator=(const remote&) = delete;

    [[nodiscard]] static runtime::task<remote> connect(remote_config config);
    [[nodiscard]] remote_blob_source& blobs() noexcept;
    [[nodiscard]] remote_executor& executor() noexcept;
    [[nodiscard]] const remote_config& config() const noexcept;
    [[nodiscard]] algorithm digest_algorithm() const noexcept;
    [[nodiscard]] runtime::task<std::shared_ptr<action_result>>
    get_action_result(repository& source, action_input input, digest sandbox);
    [[nodiscard]] runtime::task<void> update_action_result(repository& source,
                                                           action_input input,
                                                           digest sandbox,
                                                           const action_result& value);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
