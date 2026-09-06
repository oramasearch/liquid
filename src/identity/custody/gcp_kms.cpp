#include "identity/custody/providers.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

class gcp_kms_provider final : public key_provider {
public:
    explicit gcp_kms_provider(std::string resource) : resource_(std::move(resource)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        co_return co_await request(
            "encrypt", "plaintext", base64_encode(data_key), "ciphertext", context);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        auto value =
            co_await request("decrypt", "ciphertext", base64_encode(wrapped), "plaintext", context);
        if (!value || value->size() != key{}.size()) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap GCP KMS data key", reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*value, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "gcp-kms";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(resource_);
    }

private:
    auto request(std::string operation,
                 std::string field,
                 std::string value,
                 std::string response_field,
                 const wrap_context& context) -> runtime::task<std::vector<std::byte>> {
        auto token = environment("GOOGLE_OAUTH_ACCESS_TOKEN");
        if (!token) {
            co_return std::unexpected(
                custody_error(errc::unauthorized, token.error().message, reference(context)));
        }
        auto body =
            json({{std::move(field), std::move(value)},
                  {"additionalAuthenticatedData", base64_encode(as_bytes(reference(context)))}});
        if (!body) {
            co_return std::unexpected(body.error());
        }
        auto base = optional_environment("LIQUID_GCP_KMS_ENDPOINT");
        if (base.empty()) {
            base = "https://cloudkms.googleapis.com";
        }
        while (base.ends_with('/')) {
            base.pop_back();
        }
        const auto url = base + "/v1/" + resource_ + ":" + operation;
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
                custody_error(errc::corrupt, "invalid GCP KMS response", reference(context)));
        }
        auto encoded = json_string_field(*decoded, response_field);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        co_return base64_decode(*encoded, false, true);
    }

    std::string resource_;
};

} // namespace

auto make_gcp_kms_provider(std::string resource) -> result<std::shared_ptr<key_provider>> {
    if (resource.empty()) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "GCP KMS resource is empty", "gcp-kms custody"));
    }
    return std::make_shared<gcp_kms_provider>(std::move(resource));
}

} // namespace liquid::custody_detail
