// SPDX-License-Identifier: MIT

#include "identity/custody/providers.hpp"

#include <algorithm>
#include <memory>

#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

class azure_kv_provider final : public key_provider {
public:
    explicit azure_kv_provider(std::string resource) : resource_(std::move(resource)) {
        while (resource_.ends_with('/')) {
            resource_.pop_back();
        }
    }

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        co_return co_await request("wrapkey", data_key, context);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        auto value = co_await request("unwrapkey", wrapped, context);
        if (!value || value->size() != key{}.size()) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap Azure Key Vault data key", reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*value, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "azure-kv";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(resource_);
    }

private:
    auto request(std::string operation,
                 std::span<const std::byte> value,
                 const wrap_context& context) -> runtime::task<std::vector<std::byte>> {
        auto token = environment("AZURE_ACCESS_TOKEN");
        if (!token) {
            co_return std::unexpected(
                custody_error(errc::unauthorized, token.error().message, reference(context)));
        }
        auto body =
            json({{"alg", std::string{"RSA-OAEP-256"}}, {"value", base64_encode(value, true)}});
        if (!body) {
            co_return std::unexpected(body.error());
        }
        auto response = co_await perform_http(
            "POST",
            resource_ + "/" + operation + "?api-version=7.4",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + *token}},
            bytes(*body));
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto successful = require_success(std::move(*response), context);
        if (!successful) {
            co_return std::unexpected(successful.error());
        }
        auto decoded = formats::json::parse(text(successful->body));
        if (!decoded) {
            co_return std::unexpected(custody_error(
                errc::corrupt, "invalid Azure Key Vault response", reference(context)));
        }
        auto encoded = json_string_field(*decoded, "value");
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        co_return base64_decode(*encoded, true, true);
    }

    std::string resource_;
};

} // namespace

auto make_azure_kv_provider(std::string resource) -> result<std::shared_ptr<key_provider>> {
    if (!resource.starts_with("http://") && !resource.starts_with("https://")) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "Azure Key Vault URL must use HTTP or HTTPS", resource));
    }
    return std::make_shared<azure_kv_provider>(std::move(resource));
}

} // namespace liquid::custody_detail
