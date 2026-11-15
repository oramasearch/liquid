// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "formats/json.hpp"
#include "identity/custody/common.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace {

struct request {
    std::string target;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

struct fake_response {
    unsigned status{200};
    std::string body;

    fake_response(std::string value) : body(std::move(value)) {}
    fake_response(unsigned response_status, std::string value)
        : status(response_status), body(std::move(value)) {}
};

class scoped_environment {
public:
    scoped_environment(std::string name, std::string value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str()))
            previous_ = previous;
        setenv(name_.c_str(), value.c_str(), 1);
    }

    ~scoped_environment() {
        if (previous_.empty())
            unsetenv(name_.c_str());
        else
            setenv(name_.c_str(), previous_.c_str(), 1);
    }

private:
    std::string name_;
    std::string previous_;
};

auto lower(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

auto content_length(std::string_view headers) -> std::size_t {
    const auto name = std::string_view{"Content-Length:"};
    const auto position = headers.find(name);
    if (position == std::string_view::npos)
        return 0;
    return static_cast<std::size_t>(
        std::stoull(std::string{headers.substr(position + name.size())}));
}

auto send_all(int socket, std::string_view value) -> bool {
    auto sent = std::size_t{};
    while (sent != value.size()) {
        const auto count = ::send(socket, value.data() + sent, value.size() - sent, 0);
        if (count <= 0)
            return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

auto parse_request(std::string wire) -> request {
    auto output = request{};
    const auto first_space = wire.find(' ');
    const auto second_space = wire.find(' ', first_space + 1);
    output.target = wire.substr(first_space + 1, second_space - first_space - 1);
    auto position = wire.find("\r\n") + 2;
    const auto header_end = wire.find("\r\n\r\n");
    while (position < header_end) {
        const auto end = wire.find("\r\n", position);
        const auto colon = wire.find(':', position);
        auto value = wire.substr(colon + 1, end - colon - 1);
        while (!value.empty() && value.front() == ' ')
            value.erase(value.begin());
        output.headers.emplace(lower(wire.substr(position, colon - position)), std::move(value));
        position = end + 2;
    }
    output.body = wire.substr(header_end + 4);
    return output;
}

class fake_server {
public:
    using handler = std::function<fake_response(const request&)>;

    explicit fake_server(handler handle) : handle_(std::move(handle)) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("custody fake socket failed");
        auto address = sockaddr_in{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 8) != 0)
            throw std::runtime_error("custody fake bind failed");
        auto size = socklen_t{sizeof(address)};
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            throw std::runtime_error("custody fake address failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread{[this] { serve(); }};
    }

    ~fake_server() {
        stopping_.store(true);
        const auto wake = ::socket(AF_INET, SOCK_STREAM, 0);
        if (wake >= 0) {
            auto address = sockaddr_in{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            static_cast<void>(
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            ::close(wake);
        }
        thread_.join();
        ::close(listener_);
    }

    auto url() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    auto error() const -> std::string {
        auto lock = std::lock_guard{mutex_};
        return error_;
    }

private:
    auto fail(std::string message) -> void {
        auto lock = std::lock_guard{mutex_};
        if (error_.empty())
            error_ = std::move(message);
    }

    auto serve() -> void {
        while (!stopping_.load()) {
            const auto connection = ::accept(listener_, nullptr, nullptr);
            if (connection < 0)
                continue;
            if (!stopping_.load())
                handle(connection);
            ::close(connection);
        }
    }

    auto handle(int connection) -> void {
        auto wire = std::string{};
        auto expected = std::size_t{};
        for (;;) {
            auto buffer = std::array<char, 4096>{};
            const auto count = ::recv(connection, buffer.data(), buffer.size(), 0);
            if (count <= 0)
                return fail("request read failed");
            wire.append(buffer.data(), static_cast<std::size_t>(count));
            const auto header_end = wire.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                expected = header_end + 4 + content_length(wire.substr(0, header_end + 4));
                if (wire.size() >= expected)
                    break;
            }
        }
        auto reply = fake_response{""};
        try {
            reply = handle_(parse_request(std::move(wire)));
        } catch (const std::exception& exception) {
            fail(exception.what());
            reply = fake_response{500, R"({"error":"fake rejected request"})"};
        }
        const auto reason = reply.status == 200 ? "OK" : "Forbidden";
        auto response = "HTTP/1.1 " + std::to_string(reply.status) + " " + reason +
                        "\r\nContent-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n" + reply.body;
        if (!send_all(connection, response))
            fail("response write failed");
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::atomic_bool stopping_{};
    mutable std::mutex mutex_;
    std::string error_;
    handler handle_;
    std::thread thread_;
};

auto field(const request& request, std::string_view name) -> std::string {
    auto parsed = liquid::formats::json::parse(request.body);
    if (!parsed)
        throw std::runtime_error("invalid request JSON");
    auto value = liquid::custody_detail::json_string_field(*parsed, name);
    if (!value)
        throw std::runtime_error("missing request field " + std::string{name});
    return *value;
}

auto number_field(const request& request, std::string_view name) -> std::string {
    auto parsed = liquid::formats::json::parse(request.body);
    if (!parsed)
        throw std::runtime_error("invalid request JSON");
    const auto* value = liquid::custody_detail::json_field(*parsed, name);
    const auto* number = value ? std::get_if<liquid::formats::json::number>(&value->data) : nullptr;
    if (!number)
        throw std::runtime_error("missing request number " + std::string{name});
    return number->text;
}

auto response(std::string name, std::string value) -> std::string {
    auto encoded = liquid::custody_detail::json({{std::move(name), std::move(value)}});
    if (!encoded)
        throw std::runtime_error("response JSON failed");
    return *encoded;
}

auto require_header(const request& request, std::string_view name, std::string_view value) -> void {
    const auto found = request.headers.find(name);
    if (found == request.headers.end() || found->second != value)
        throw std::runtime_error("invalid " + std::string{name} + " header");
}

auto signing_key(std::string_view secret, std::string_view date)
    -> liquid::custody_detail::sha256_digest {
    const auto initial = "AWS4" + std::string{secret};
    const auto dated = liquid::custody_detail::hmac_sha256(
        liquid::custody_detail::as_bytes(initial), liquid::custody_detail::as_bytes(date));
    const auto regional =
        liquid::custody_detail::hmac_sha256(dated, liquid::custody_detail::as_bytes("us-west-2"));
    const auto service =
        liquid::custody_detail::hmac_sha256(regional, liquid::custody_detail::as_bytes("kms"));
    return liquid::custody_detail::hmac_sha256(service,
                                               liquid::custody_detail::as_bytes("aws4_request"));
}

auto verify_aws_signature(const request& request) -> void {
    const auto date = request.headers.at("x-amz-date");
    const auto target = request.headers.at("x-amz-target");
    const auto host = request.headers.at("host");
    const auto signed_headers = "content-type;host;x-amz-date;x-amz-target";
    auto canonical_headers = std::string{"content-type:application/x-amz-json-1.1\n"};
    canonical_headers += "host:" + host + "\n";
    canonical_headers += "x-amz-date:" + date + "\n";
    canonical_headers += "x-amz-target:" + target + "\n";
    const auto canonical = "POST\n/\n\n" + canonical_headers + "\n" + signed_headers + "\n" +
                           liquid::custody_detail::hex(liquid::custody_detail::sha256(
                               liquid::custody_detail::as_bytes(request.body)));
    const auto scope = date.substr(0, 8) + "/us-west-2/kms/aws4_request";
    const auto to_sign = "AWS4-HMAC-SHA256\n" + date + "\n" + scope + "\n" +
                         liquid::custody_detail::hex(liquid::custody_detail::sha256(
                             liquid::custody_detail::as_bytes(canonical)));
    const auto signature = liquid::custody_detail::hex(liquid::custody_detail::hmac_sha256(
        signing_key("SECRET", date.substr(0, 8)), liquid::custody_detail::as_bytes(to_sign)));
    const auto expected = "AWS4-HMAC-SHA256 Credential=AKID/" + scope +
                          ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
    require_header(request, "authorization", expected);
}

auto round_trip(std::shared_ptr<liquid::key_provider> provider) -> void {
    auto source = liquid::key{};
    source[0] = std::byte{0x51};
    source[31] = std::byte{0x17};
    auto wrapped = liquid::runtime::block_on(provider->wrap(source, {"remote", 3}));
    ASSERT_TRUE(wrapped) << wrapped.error().message;
    auto opened = liquid::runtime::block_on(provider->unwrap(*wrapped, {"remote", 3}));
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, source);
}

} // namespace

TEST(RemoteCustody, AwsKmsUsesSignedJsonEncryptAndDecrypt) {
    fake_server server{[](const request& request) {
        verify_aws_signature(request);
        auto parsed = liquid::formats::json::parse(request.body);
        if (!parsed)
            throw std::runtime_error("invalid AWS request JSON");
        const auto* encryption_context =
            liquid::custody_detail::json_field(*parsed, "EncryptionContext");
        if (!encryption_context)
            throw std::runtime_error("missing AWS encryption context");
        auto context_key =
            liquid::custody_detail::json_string_field(*encryption_context, "liquid-key-id");
        auto context_version =
            liquid::custody_detail::json_string_field(*encryption_context, "liquid-version");
        if (!context_key || *context_key != "remote" || !context_version || *context_version != "3")
            throw std::runtime_error("wrong AWS encryption context");
        if (request.headers.at("x-amz-target") == "TrentService.Encrypt") {
            if (field(request, "KeyId") != "arn:aws:kms:us-west-2:123:key/abc")
                throw std::runtime_error("wrong AWS key id");
            return response("CiphertextBlob", field(request, "Plaintext"));
        }
        return response("Plaintext", field(request, "CiphertextBlob"));
    }};
    auto access = scoped_environment{"AWS_ACCESS_KEY_ID", "AKID"};
    auto secret = scoped_environment{"AWS_SECRET_ACCESS_KEY", "SECRET"};
    auto endpoint = scoped_environment{"LIQUID_AWS_KMS_ENDPOINT", server.url() + "/"};
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("aws-kms:arn:aws:kms:us-west-2:123:key/abc"));
    ASSERT_TRUE(provider) << provider.error().message;
    round_trip(*provider);
    EXPECT_EQ(server.error(), "");
}

TEST(RemoteCustody, GcpKmsUsesBearerAndCanonicalOperations) {
    fake_server server{[](const request& request) {
        require_header(request, "authorization", "Bearer gcp-token");
        if (field(request, "additionalAuthenticatedData") != "cmVtb3RlQDM")
            throw std::runtime_error("wrong GCP authenticated data");
        if (request.target.ends_with(":encrypt")) {
            if (!request.target.starts_with("/v1/projects/p/locations/l/"))
                throw std::runtime_error("wrong GCP resource path");
            return response("ciphertext", field(request, "plaintext"));
        }
        if (!request.target.ends_with(":decrypt"))
            throw std::runtime_error("wrong GCP operation");
        return response("plaintext", field(request, "ciphertext"));
    }};
    auto token = scoped_environment{"GOOGLE_OAUTH_ACCESS_TOKEN", "gcp-token"};
    auto endpoint = scoped_environment{"LIQUID_GCP_KMS_ENDPOINT", server.url()};
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("gcp-kms:projects/p/locations/l/keyRings/r/cryptoKeys/k"));
    ASSERT_TRUE(provider) << provider.error().message;
    round_trip(*provider);
    EXPECT_EQ(server.error(), "");
}

TEST(RemoteCustody, AzureKeyVaultUsesBearerWrapkeyAndUnwrapkey) {
    fake_server server{[](const request& request) {
        require_header(request, "authorization", "Bearer azure-token");
        if (request.target != "/keys/key/version/wrapkey?api-version=7.4" &&
            request.target != "/keys/key/version/unwrapkey?api-version=7.4")
            throw std::runtime_error("wrong Azure operation path");
        if (field(request, "alg") != "RSA-OAEP-256")
            throw std::runtime_error("wrong Azure algorithm");
        return response("value", field(request, "value"));
    }};
    auto token = scoped_environment{"AZURE_ACCESS_TOKEN", "azure-token"};
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("azure-kv:" + server.url() + "/keys/key/version"));
    ASSERT_TRUE(provider) << provider.error().message;
    round_trip(*provider);
    EXPECT_EQ(server.error(), "");
}

TEST(RemoteCustody, LakeTransitUsesBearerAndCustodyRoutes) {
    fake_server server{[](const request& request) {
        require_header(request, "authorization", "Bearer liquid-token");
        if (field(request, "key_id") != "remote" || number_field(request, "version") != "3")
            throw std::runtime_error("wrong lake wrap context");
        if (request.target == "/tenant/v1/custody/team/wrap") {
            const auto plaintext = field(request, "plaintext");
            if (request.body !=
                "{\"key_id\":\"remote\",\"plaintext\":\"" + plaintext + "\",\"version\":3}")
                throw std::runtime_error("lake wrap JSON is not canonical");
            return response("wrapped", plaintext);
        }
        if (request.target == "/tenant/v1/custody/team/unwrap") {
            const auto wrapped = field(request, "wrapped");
            if (request.body !=
                "{\"key_id\":\"remote\",\"version\":3,\"wrapped\":\"" + wrapped + "\"}")
                throw std::runtime_error("lake unwrap JSON is not canonical");
            return response("plaintext", wrapped);
        }
        throw std::runtime_error("wrong lake custody path");
    }};
    auto token = scoped_environment{"LIQUID_TOKEN", "liquid-token"};
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("lake:" + server.url() + "/tenant/team"));
    ASSERT_TRUE(provider) << provider.error().message;
    round_trip(*provider);
    EXPECT_EQ(server.error(), "");
}

TEST(RemoteCustody, MissingCredentialIsUnauthorizedForTheVersion) {
    auto missing = scoped_environment{"GOOGLE_OAUTH_ACCESS_TOKEN", ""};
    auto provider = liquid::runtime::block_on(
        liquid::key_provider_from_uri("gcp-kms:projects/p/locations/l/keyRings/r/cryptoKeys/k"));
    ASSERT_TRUE(provider) << provider.error().message;
    auto data_key = liquid::key{};
    auto refused = liquid::runtime::block_on((*provider)->wrap(data_key, {"remote", 9}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(refused.error().context, "remote@9");
}

TEST(RemoteCustody, RejectedCredentialsAreUnauthorizedForTheVersion) {
    fake_server server{
        [](const request&) { return fake_response{403, R"({"error":"credential rejected"})"}; }};
    auto aws_access = scoped_environment{"AWS_ACCESS_KEY_ID", "rejected"};
    auto aws_secret = scoped_environment{"AWS_SECRET_ACCESS_KEY", "rejected"};
    auto aws_endpoint = scoped_environment{"LIQUID_AWS_KMS_ENDPOINT", server.url() + "/"};
    auto gcp_token = scoped_environment{"GOOGLE_OAUTH_ACCESS_TOKEN", "rejected"};
    auto gcp_endpoint = scoped_environment{"LIQUID_GCP_KMS_ENDPOINT", server.url()};
    auto azure_token = scoped_environment{"AZURE_ACCESS_TOKEN", "rejected"};
    auto lake_token = scoped_environment{"LIQUID_TOKEN", "rejected"};
    const auto uris = std::array{
        std::string{"aws-kms:arn:aws:kms:us-west-2:123:key/abc"},
        std::string{"gcp-kms:projects/p/locations/l/keyRings/r/cryptoKeys/k"},
        std::string{"azure-kv:"} + server.url() + "/keys/key/version",
        std::string{"lake:"} + server.url() + "/tenant/team",
    };
    for (const auto& uri : uris) {
        auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri(uri));
        ASSERT_TRUE(provider) << provider.error().message;
        auto data_key = liquid::key{};
        auto refused = liquid::runtime::block_on((*provider)->wrap(data_key, {"remote", 11}));
        ASSERT_FALSE(refused) << uri;
        EXPECT_EQ(refused.error().code, liquid::errc::unauthorized) << uri;
        EXPECT_EQ(refused.error().context, "remote@11") << uri;
    }
    EXPECT_EQ(server.error(), "");
}
