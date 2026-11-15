#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "liquid/listener.hpp"
#include "liquid/reapi.hpp"
#include "liquid/runtime.hpp"
#include "temporary_directory.hpp"

namespace {

const auto token = std::string(40, 'k');

auto with_tls() -> liquid::listener_security {
    auto security = liquid::listener_security{};
    security.tls_certificate = std::filesystem::path{"certificate.pem"};
    security.tls_key = std::filesystem::path{"key.pem"};
    return security;
}

class token_directory {
public:
    token_directory()
        : path_(liquid::test::fixture_path("liquid-listener-token-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_);
    }

    ~token_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto write(const std::string& name, const std::string& contents) const
        -> std::filesystem::path {
        const auto file = path_ / name;
        {
            auto output = std::ofstream{file, std::ios::binary};
            output << contents;
        }
        std::filesystem::permissions(file,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);
        return file;
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static int next_{};
    std::filesystem::path path_;
};

} // namespace

TEST(ListenerSecurity, OnlyLoopbackAddressesAreLoopback) {
    EXPECT_TRUE(liquid::is_loopback_address("127.0.0.1"));
    EXPECT_TRUE(liquid::is_loopback_address("127.10.20.30"));
    EXPECT_TRUE(liquid::is_loopback_address("::1"));
    EXPECT_TRUE(liquid::is_loopback_address("[::1]"));
    EXPECT_FALSE(liquid::is_loopback_address("0.0.0.0"));
    EXPECT_FALSE(liquid::is_loopback_address("::"));
    EXPECT_FALSE(liquid::is_loopback_address("[::]"));
    EXPECT_FALSE(liquid::is_loopback_address("10.0.0.1"));
    EXPECT_FALSE(liquid::is_loopback_address("192.168.1.10"));
    EXPECT_FALSE(liquid::is_loopback_address("localhost"));
    EXPECT_FALSE(liquid::is_loopback_address("::ffff:127.0.0.1"));
    EXPECT_FALSE(liquid::is_loopback_address(""));
}

TEST(ListenerSecurity, LoopbackNeedsNoTlsOrAuthentication) {
    EXPECT_TRUE(liquid::validate_listener_security(true, {}));
    auto security = liquid::listener_security{};
    security.bearer_token = token;
    EXPECT_TRUE(liquid::validate_listener_security(true, security));
}

TEST(ListenerSecurity, NonLoopbackRequiresTlsAndAuthentication) {
    auto refused = liquid::validate_listener_security(false, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);

    auto token_only = liquid::listener_security{};
    token_only.bearer_token = token;
    EXPECT_FALSE(liquid::validate_listener_security(false, token_only));

    EXPECT_FALSE(liquid::validate_listener_security(false, with_tls()));

    auto tls_and_token = with_tls();
    tls_and_token.bearer_token = token;
    EXPECT_TRUE(liquid::validate_listener_security(false, tls_and_token));

    auto mutual = with_tls();
    mutual.client_certificate_authority = std::filesystem::path{"clients.pem"};
    EXPECT_TRUE(liquid::validate_listener_security(false, mutual));
}

TEST(ListenerSecurity, RefusesIncompleteOrWeakConfiguration) {
    auto certificate_only = liquid::listener_security{};
    certificate_only.tls_certificate = std::filesystem::path{"certificate.pem"};
    EXPECT_FALSE(liquid::validate_listener_security(true, certificate_only));

    auto authority_without_tls = liquid::listener_security{};
    authority_without_tls.client_certificate_authority = std::filesystem::path{"clients.pem"};
    EXPECT_FALSE(liquid::validate_listener_security(true, authority_without_tls));

    auto short_token = with_tls();
    short_token.bearer_token = std::string(liquid::minimum_bearer_token_bytes - 1, 'k');
    EXPECT_FALSE(liquid::validate_listener_security(false, short_token));

    auto spaced_token = with_tls();
    spaced_token.bearer_token = std::string(20, 'k') + " " + std::string(20, 'k');
    EXPECT_FALSE(liquid::validate_listener_security(false, spaced_token));
}

TEST(ListenerSecurity, BearerComparisonAcceptsOnlyTheExactToken) {
    EXPECT_TRUE(liquid::bearer_authorized(token, "Bearer " + token));
    EXPECT_TRUE(liquid::bearer_authorized(token, "bearer " + token));
    EXPECT_FALSE(liquid::bearer_authorized(token, "Bearer " + token + "x"));
    EXPECT_FALSE(liquid::bearer_authorized(token, "Bearer " + token.substr(1)));
    EXPECT_FALSE(liquid::bearer_authorized(token, "Bearer " + std::string(40, 'j')));
    EXPECT_FALSE(liquid::bearer_authorized(token, token));
    EXPECT_FALSE(liquid::bearer_authorized(token, "Basic " + token));
    EXPECT_FALSE(liquid::bearer_authorized(token, "Bearer "));
    EXPECT_FALSE(liquid::bearer_authorized(token, ""));
    EXPECT_FALSE(liquid::bearer_authorized("", "Bearer "));
}

#if !defined(_WIN32)
TEST(ListenerSecurity, TokenFileMustBeReadableOnlyByItsOwner) {
    const auto directory = token_directory{};
    const auto private_file = directory.write("token", token + "\n");
    auto read = liquid::read_token_file(private_file);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, token);

    const auto shared_file = directory.write("shared", token);
    std::filesystem::permissions(
        shared_file, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    auto shared = liquid::read_token_file(shared_file);
    ASSERT_FALSE(shared);
    EXPECT_EQ(shared.error().code, liquid::errc::invalid_argument);

    const auto empty_file = directory.write("empty", "\n");
    EXPECT_FALSE(liquid::read_token_file(empty_file));
    EXPECT_FALSE(liquid::read_token_file(directory.path() / "missing"));
    EXPECT_FALSE(liquid::read_token_file(directory.path()));
    const auto link = directory.path() / "link";
    std::filesystem::create_symlink(private_file, link);
    EXPECT_FALSE(liquid::read_token_file(link));
}
#endif

TEST(ListenerSecurity, ReapiServerRefusesAPlaintextNonLoopbackListener) {
    auto options =
        liquid::reapi_serve_options{.store_directory = "unused-store", .listen = "0.0.0.0:50051"};
    auto served = liquid::runtime::block_on(liquid::serve_reapi(options));
    ASSERT_FALSE(served);
    EXPECT_EQ(served.error().code, liquid::errc::invalid_argument);

    options.listen = "[::]:50051";
    options.security.bearer_token = token;
    served = liquid::runtime::block_on(liquid::serve_reapi(options));
    ASSERT_FALSE(served);
    EXPECT_EQ(served.error().code, liquid::errc::invalid_argument);

    options.listen = "10.0.0.1:50051";
    options.security = with_tls();
    served = liquid::runtime::block_on(liquid::serve_reapi(options));
    ASSERT_FALSE(served);
    EXPECT_EQ(served.error().code, liquid::errc::invalid_argument);
}
