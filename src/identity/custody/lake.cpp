// SPDX-License-Identifier: MIT

#include "identity/custody/providers.hpp"

#include "credential_token.hpp"

#include <algorithm>
#include <memory>

#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

auto lake_body(std::string_view value_name,
               std::span<const std::byte> value,
               const wrap_context& context) -> result<std::string> {
    auto key_id = formats::json::write(formats::json::value{context.key_id});
    if (!key_id)
        return std::unexpected(key_id.error());
    if (key_id->ends_with('\n'))
        key_id->pop_back();
    const auto encoded = base64_encode(value);
    if (value_name == "plaintext") {
        return "{\"key_id\":" + *key_id + ",\"plaintext\":\"" + encoded +
               "\",\"version\":" + std::to_string(context.version) + "}";
    }
    return "{\"key_id\":" + *key_id + ",\"version\":" + std::to_string(context.version) +
           ",\"wrapped\":\"" + encoded + "\"}";
}

class lake_provider final : public key_provider {
public:
    lake_provider(std::string resource, std::string base, std::string key_name)
        : resource_(std::move(resource)), base_(std::move(base)), key_name_(std::move(key_name)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        auto body = lake_body("plaintext", data_key, context);
        co_return co_await request("wrap", "wrapped", std::move(body), context);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        auto body = lake_body("wrapped", wrapped, context);
        auto value = co_await request("unwrap", "plaintext", std::move(body), context);
        if (!value || value->size() != key{}.size()) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap lake transit data key", reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*value, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "lake";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(resource_);
    }

private:
    auto request(std::string operation,
                 std::string response_field,
                 result<std::string> body,
                 const wrap_context& context) -> runtime::task<std::vector<std::byte>> {
        if (!body) {
            co_return std::unexpected(body.error());
        }
        // LIQUID_TOKEN_FILE or LIQUID_TOKEN (liquid#410).
        auto token = std::optional<std::string>{token_detail::lake_token()};
        if (token->empty()) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "LIQUID_TOKEN_FILE or LIQUID_TOKEN is not set", reference(context)));
        }
        const auto url = base_ + "/v1/custody/" + key_name_ + "/" + operation;
        auto response = co_await perform_http(
            "POST",
            url,
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
            co_return std::unexpected(
                custody_error(errc::corrupt, "invalid lake transit response", reference(context)));
        }
        auto encoded = json_string_field(*decoded, response_field);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        co_return base64_decode(*encoded, false, true);
    }

    std::string resource_;
    std::string base_;
    std::string key_name_;
};

} // namespace

auto make_lake_provider(std::string resource) -> result<std::shared_ptr<key_provider>> {
    const auto separator = resource.rfind('/');
    if ((!resource.starts_with("http://") && !resource.starts_with("https://")) ||
        separator == std::string::npos || separator < resource.find("//") + 2 ||
        separator + 1 == resource.size()) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid lake custody URL and key", resource));
    }
    auto base = resource.substr(0, separator);
    auto key_name = resource.substr(separator + 1);
    return std::make_shared<lake_provider>(
        std::move(resource), std::move(base), std::move(key_name));
}

} // namespace liquid::custody_detail
