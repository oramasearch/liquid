#include "identity/custody/providers.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string_view>

#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

struct endpoint_parts {
    std::string host;
    std::string path;
    std::string query;
};

auto endpoint(std::string_view url) -> result<endpoint_parts> {
    const auto scheme = url.find("//");
    if (scheme == std::string_view::npos) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid AWS KMS endpoint", std::string{url}));
    }
    url.remove_prefix(scheme + 2);
    const auto slash = url.find('/');
    auto output = endpoint_parts{};
    output.host = url.substr(0, slash);
    auto target = slash == std::string_view::npos ? std::string_view{"/"} : url.substr(slash);
    const auto question = target.find('?');
    output.path = target.substr(0, question);
    output.query = question == std::string_view::npos ? std::string{}
                                                      : std::string{target.substr(question + 1)};
    return output;
}

auto region_from_arn(std::string_view arn) -> result<std::string> {
    auto position = std::size_t{};
    for (auto field = 0; field != 3; ++field) {
        position = arn.find(':', position);
        if (position == std::string_view::npos) {
            return std::unexpected(
                custody_error(errc::invalid_argument, "invalid AWS KMS ARN", std::string{arn}));
        }
        ++position;
    }
    const auto end = arn.find(':', position);
    if (end == std::string_view::npos || end == position) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid AWS KMS ARN", std::string{arn}));
    }
    return std::string{arn.substr(position, end - position)};
}

auto signing_key(std::string_view secret, std::string_view date, std::string_view region)
    -> sha256_digest {
    const auto initial = "AWS4" + std::string{secret};
    const auto dated = hmac_sha256(as_bytes(initial), as_bytes(date));
    const auto regional = hmac_sha256(dated, as_bytes(region));
    const auto service = hmac_sha256(regional, as_bytes("kms"));
    return hmac_sha256(service, as_bytes("aws4_request"));
}

class aws_kms_provider final : public key_provider {
public:
    aws_kms_provider(std::string resource, std::string region)
        : resource_(std::move(resource)), region_(std::move(region)) {}

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        auto body = json({{"KeyId", resource_},
                          {"Plaintext", base64_encode(data_key)},
                          {"EncryptionContext",
                           formats::json::value{formats::json::value::object{
                               {"liquid-key-id", context.key_id},
                               {"liquid-version", std::to_string(context.version)}}}}});
        co_return co_await request("Encrypt", "CiphertextBlob", std::move(body), context);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        auto body = json({{"KeyId", resource_},
                          {"CiphertextBlob", base64_encode(wrapped)},
                          {"EncryptionContext",
                           formats::json::value{formats::json::value::object{
                               {"liquid-key-id", context.key_id},
                               {"liquid-version", std::to_string(context.version)}}}}});
        auto value = co_await request("Decrypt", "Plaintext", std::move(body), context);
        if (!value || value->size() != key{}.size()) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "cannot unwrap AWS KMS data key", reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*value, output.begin());
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "aws-kms";
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
        auto access_key = environment("AWS_ACCESS_KEY_ID");
        auto secret_key = environment("AWS_SECRET_ACCESS_KEY");
        if (!access_key || !secret_key) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "AWS KMS credentials are unavailable", reference(context)));
        }
        auto url = optional_environment("LIQUID_AWS_KMS_ENDPOINT");
        if (url.empty()) {
            url = "https://kms." + region_ + ".amazonaws.com/";
        }
        auto parts = endpoint(url);
        if (!parts) {
            co_return std::unexpected(parts.error());
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto timestamp = std::format("{:%Y%m%dT%H%M%SZ}", now);
        const auto date = timestamp.substr(0, 8);
        const auto target = "TrentService." + operation;
        const auto session_token = optional_environment("AWS_SESSION_TOKEN");
        auto canonical_headers = std::string{"content-type:application/x-amz-json-1.1\n"};
        canonical_headers += "host:" + parts->host + "\n";
        canonical_headers += "x-amz-date:" + timestamp + "\n";
        if (!session_token.empty()) {
            canonical_headers += "x-amz-security-token:" + session_token + "\n";
        }
        canonical_headers += "x-amz-target:" + target + "\n";
        auto signed_headers = std::string{"content-type;host;x-amz-date;"};
        if (!session_token.empty()) {
            signed_headers += "x-amz-security-token;";
        }
        signed_headers += "x-amz-target";
        const auto canonical = "POST\n" + parts->path + "\n" + parts->query + "\n" +
                               canonical_headers + "\n" + signed_headers + "\n" +
                               hex(sha256(as_bytes(*body)));
        const auto scope = date + "/" + region_ + "/kms/aws4_request";
        const auto string_to_sign = "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                    hex(sha256(as_bytes(canonical)));
        const auto signature =
            hex(hmac_sha256(signing_key(*secret_key, date, region_), as_bytes(string_to_sign)));
        const auto authorization = "AWS4-HMAC-SHA256 Credential=" + *access_key + "/" + scope +
                                   ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
        auto headers = http_headers{{"Content-Type", "application/x-amz-json-1.1"},
                                    {"X-Amz-Date", timestamp},
                                    {"X-Amz-Target", target},
                                    {"Authorization", authorization}};
        if (!session_token.empty()) {
            headers.emplace_back("X-Amz-Security-Token", session_token);
        }
        auto response = co_await perform_http("POST", url, std::move(headers), bytes(*body));
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
                custody_error(errc::corrupt, "invalid AWS KMS response", reference(context)));
        }
        auto encoded = json_string_field(*decoded, response_field);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        co_return base64_decode(*encoded, false, true);
    }

    std::string resource_;
    std::string region_;
};

} // namespace

auto make_aws_kms_provider(std::string resource) -> result<std::shared_ptr<key_provider>> {
    auto region = region_from_arn(resource);
    if (!region) {
        return std::unexpected(region.error());
    }
    return std::make_shared<aws_kms_provider>(std::move(resource), std::move(*region));
}

} // namespace liquid::custody_detail
