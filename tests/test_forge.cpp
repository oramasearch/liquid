#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "liquid/forge.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "support/forge_double.hpp"
#include "temporary_directory.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

class repository_fixture {
public:
    repository_fixture()
        : path_(liquid::test::fixture_path("forge-repository-" + std::to_string(next_++))) {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    ~repository_fixture() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto make_change(liquid::repository& repository,
                 std::string path,
                 std::string contents,
                 std::string message) -> liquid::result<liquid::change_id> {
    auto written = liquid::runtime::block_on(repository.write(std::move(path), bytes(contents)));
    if (!written) {
        return std::unexpected(written.error());
    }
    return liquid::runtime::block_on(repository.cut(std::move(message)));
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output += character;
        }
    }
    output += '\'';
    return output;
}

auto read_file(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
#endif

} // namespace

TEST(Forge, review_comment_and_state_round_trips_use_the_lake_envelope) {
    auto server = liquid::test::forge_double{"token-value"};
    auto client = liquid::forge_client{
        server.url(),
        {.bearer_token = "token-value", .poll_interval = std::chrono::milliseconds{1}}};
    auto digest = liquid::parse_digest(
        "blake3:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    ASSERT_TRUE(digest);
    const auto change = liquid::change_id{*digest};

    auto created = liquid::runtime::block_on(client.create_review(change, {"alice", "bob"}));
    ASSERT_TRUE(created);
    EXPECT_EQ(created->state, liquid::forge_review_state::open);
    EXPECT_EQ(created->reviewers, (std::vector<std::string>{"alice", "bob"}));

    auto listed = liquid::runtime::block_on(client.list_reviews(liquid::forge_review_state::open));
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ(listed->front().id, created->id);

    auto comment = liquid::runtime::block_on(
        client.create_comment(created->id,
                              "please keep this",
                              {.change = change, .path = "src/file.cpp", .line = 7, .column = 3}));
    ASSERT_TRUE(comment);
    EXPECT_EQ(comment->review, created->id);
    EXPECT_EQ(comment->anchor.line, 7U);

    auto comments = liquid::runtime::block_on(client.list_comments(created->id));
    ASSERT_TRUE(comments);
    ASSERT_EQ(comments->size(), 1U);
    EXPECT_EQ(comments->front().body, "please keep this");

    auto approved = liquid::runtime::block_on(
        client.set_review_state(created->id, liquid::forge_review_state::approved));
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->state, liquid::forge_review_state::approved);
    auto closed = liquid::runtime::block_on(
        client.set_review_state(created->id, liquid::forge_review_state::closed));
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->state, liquid::forge_review_state::closed);
    EXPECT_EQ(server.last_authorization(), "Bearer token-value");
}

TEST(Forge, maps_every_lake_error_code_without_exposing_the_bearer) {
    auto server = liquid::test::forge_double{"do-not-print-this"};
    auto client = liquid::forge_client{server.url(), {.bearer_token = "do-not-print-this"}};
    const auto cases = std::to_array<std::pair<std::string_view, liquid::errc>>({
        {"invalid_input", liquid::errc::invalid_argument},
        {"unauthorized", liquid::errc::unauthorized},
        {"mfa_required", liquid::errc::unauthorized},
        {"not_found", liquid::errc::not_found},
        {"conflict", liquid::errc::conflict},
        {"store_unavailable", liquid::errc::unavailable},
        {"unexpected_code", liquid::errc::internal},
    });
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    for (const auto& [wire, expected] : cases) {
        server.fail_next(std::string{wire}, "preserved lake message", "/review_id");
        auto result = liquid::runtime::block_on(client.list_reviews());
        ASSERT_FALSE(result) << wire;
        EXPECT_EQ(result.error().code, expected) << wire;
        EXPECT_EQ(result.error().message, "preserved lake message") << wire;
        EXPECT_EQ(result.error().context, "/review_id") << wire;
        EXPECT_EQ(result.error().message.find("do-not-print-this"), std::string::npos);
        EXPECT_EQ(result.error().context.find("do-not-print-this"), std::string::npos);
    }
    const auto standard_error = testing::internal::GetCapturedStderr();
    const auto standard_output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(standard_error.find("do-not-print-this"), std::string::npos);
    EXPECT_EQ(standard_output.find("do-not-print-this"), std::string::npos);
}

TEST(Forge, returns_branch_protection_values) {
    auto server = liquid::test::forge_double{};
    server.add_branch_protection({.pattern = "refs/heads/main",
                                  .required_review = true,
                                  .required_verification = true,
                                  .fast_forward_only = true,
                                  .allowed_pushers = {"alice"}});
    auto client = liquid::forge_client{server.url()};
    auto rules = liquid::runtime::block_on(client.branch_protections());
    ASSERT_TRUE(rules);
    ASSERT_EQ(rules->size(), 1U);
    EXPECT_EQ(rules->front().pattern, "refs/heads/main");
    EXPECT_TRUE(rules->front().required_review);
    EXPECT_EQ(rules->front().allowed_pushers, (std::vector<std::string>{"alice"}));
}

#if !defined(_WIN32)
TEST(Forge, verifies_the_committed_certificate_over_tls) {
    const auto* previous = std::getenv("SSL_CERT_FILE");
    const auto saved =
        previous == nullptr ? std::optional<std::string>{} : std::optional<std::string>{previous};
    ASSERT_EQ(setenv("SSL_CERT_FILE", LIQUID_TLS_CERTIFICATE, 1), 0);
    auto server = liquid::test::forge_double{
        "tls-token", "127.0.0.1:0", LIQUID_TLS_CERTIFICATE, LIQUID_TLS_PRIVATE_KEY};
    auto client = liquid::forge_client{server.url(), {.bearer_token = "tls-token"}};
    auto reviews = liquid::runtime::block_on(client.list_reviews());
    if (saved) {
        static_cast<void>(setenv("SSL_CERT_FILE", saved->c_str(), 1));
    } else {
        static_cast<void>(unsetenv("SSL_CERT_FILE"));
    }
    ASSERT_TRUE(reviews) << reviews.error().message << " (" << reviews.error().context << ')';
    EXPECT_TRUE(reviews->empty());
}
// liquid#368: https from macOS failed with "unspecified system_category error". Corosio
// never applied set_default_verify_paths(), loaded only the first certificate of a CA
// bundle, and reported OpenSSL failures as errno values. These go through the same
// http1 client every https request uses, sync included.
class scoped_environment final {
public:
    explicit scoped_environment(
        std::vector<std::pair<std::string, std::optional<std::string>>> values) {
        for (auto& [name, value] : values) {
            const auto* previous = std::getenv(name.c_str());
            saved_.emplace_back(name,
                                previous == nullptr ? std::optional<std::string>{}
                                                    : std::optional<std::string>{previous});
            assign(name, value);
        }
    }
    scoped_environment(const scoped_environment&) = delete;
    auto operator=(const scoped_environment&) -> scoped_environment& = delete;
    ~scoped_environment() {
        for (const auto& [name, value] : saved_) {
            assign(name, value);
        }
    }

private:
    static void assign(const std::string& name, const std::optional<std::string>& value) {
        if (value) {
            static_cast<void>(setenv(name.c_str(), value->c_str(), 1));
        } else {
            static_cast<void>(unsetenv(name.c_str()));
        }
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto list_reviews_over_tls() -> liquid::result<std::vector<liquid::forge_review>> {
    auto server = liquid::test::forge_double{
        "tls-token", "127.0.0.1:0", LIQUID_TLS_CERTIFICATE, LIQUID_TLS_PRIVATE_KEY};
    auto client = liquid::forge_client{server.url(), {.bearer_token = "tls-token"}};
    return liquid::runtime::block_on(client.list_reviews());
}

TEST(Forge, verifies_through_the_default_trust_store) {
    // Without SSL_CERT_FILE the client asks for OpenSSL's default store, which honours
    // SSL_CERT_DIR: a hashed directory holding the test certificate.
    const auto directory = liquid::test::fixture_path("forge-default-trust");
    auto ignored = std::error_code{};
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    auto* file = std::fopen(LIQUID_TLS_CERTIFICATE, "rb");
    ASSERT_NE(file, nullptr);
    auto* certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
    std::fclose(file);
    ASSERT_NE(certificate, nullptr);
    auto name = std::array<char, 16>{};
    std::snprintf(name.data(), name.size(), "%08lx.0", X509_subject_name_hash(certificate));
    X509_free(certificate);
    std::filesystem::copy_file(LIQUID_TLS_CERTIFICATE, directory / name.data());

    auto reviews = [&] {
        auto environment = scoped_environment{
            {{"SSL_CERT_FILE", std::nullopt}, {"SSL_CERT_DIR", directory.string()}}};
        return list_reviews_over_tls();
    }();
    std::filesystem::remove_all(directory, ignored);
    ASSERT_TRUE(reviews) << reviews.error().message << " (" << reviews.error().context << ')';
    EXPECT_TRUE(reviews->empty());
}

TEST(Forge, verifies_against_a_certificate_bundle) {
    // A system bundle (/etc/ssl/cert.pem) holds many roots; the one that signs the server
    // is rarely the first.
    const auto bundle = liquid::test::fixture_path("forge-bundle.pem");
    {
        auto output = std::ofstream{bundle, std::ios::binary};
        output << read_text(LIQUID_TLS_OTHER_CERTIFICATE) << read_text(LIQUID_TLS_CERTIFICATE);
    }
    auto reviews = [&] {
        auto environment = scoped_environment{{{"SSL_CERT_FILE", bundle.string()}}};
        return list_reviews_over_tls();
    }();
    auto ignored = std::error_code{};
    std::filesystem::remove(bundle, ignored);
    ASSERT_TRUE(reviews) << reviews.error().message << " (" << reviews.error().context << ')';
    EXPECT_TRUE(reviews->empty());
}

TEST(Forge, says_why_certificate_verification_failed) {
    auto reviews = [&] {
        auto environment =
            scoped_environment{{{"SSL_CERT_FILE", std::string{LIQUID_TLS_OTHER_CERTIFICATE}}}};
        return list_reviews_over_tls();
    }();
    ASSERT_FALSE(reviews);
    EXPECT_EQ(reviews.error().code, liquid::errc::unavailable);
    EXPECT_NE(reviews.error().message.find("certificate verify failed"), std::string::npos)
        << reviews.error().message;
    EXPECT_EQ(reviews.error().message.find("unspecified"), std::string::npos)
        << reviews.error().message;
}
#endif

TEST(Forge, advance_anchors_posts_repository_line_hunks) {
    repository_fixture location;
    auto repository = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(repository);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("shifted.txt", bytes("one\ncomment\nthree\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("deleted.txt", bytes("one\nremove me\nthree\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("approximate.txt", bytes("one\nreplace me\nthree\n"))));
    auto source = liquid::runtime::block_on(repository->cut("source"));
    ASSERT_TRUE(source);

    auto server = liquid::test::forge_double{};
    auto client = liquid::forge_client{server.url()};
    auto review = liquid::runtime::block_on(client.create_review(*source));
    ASSERT_TRUE(review);
    const auto anchors = std::to_array<std::pair<std::string_view, std::uint64_t>>({
        {"shifted.txt", 2},
        {"deleted.txt", 2},
        {"approximate.txt", 2},
    });
    for (const auto& [path, line] : anchors) {
        auto comment = liquid::runtime::block_on(client.create_comment(
            review->id,
            std::string{path},
            {.change = *source, .path = std::string{path}, .line = line, .column = 1}));
        ASSERT_TRUE(comment);
    }

    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("shifted.txt", bytes("inserted\none\ncomment\nthree\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository->write("deleted.txt", bytes("one\nthree\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository->write("approximate.txt", bytes("one\nreplacement\nthree\n"))));
    auto target = liquid::runtime::block_on(repository->cut("target"));
    ASSERT_TRUE(target);

    auto mapped =
        liquid::runtime::block_on(client.advance_anchors(*repository, review->id, *target));
    ASSERT_TRUE(mapped);
    ASSERT_EQ(mapped->size(), 3U);
    const auto shifted =
        std::ranges::find(*mapped, "shifted.txt", [](const auto& comment) { return comment.body; });
    const auto deleted =
        std::ranges::find(*mapped, "deleted.txt", [](const auto& comment) { return comment.body; });
    const auto approximate = std::ranges::find(
        *mapped, "approximate.txt", [](const auto& comment) { return comment.body; });
    ASSERT_NE(shifted, mapped->end());
    ASSERT_NE(deleted, mapped->end());
    ASSERT_NE(approximate, mapped->end());
    ASSERT_TRUE(shifted->mapped);
    EXPECT_TRUE(shifted->mapped->moved);
    EXPECT_EQ(shifted->anchor.line, 3U);
    ASSERT_TRUE(deleted->mapped);
    EXPECT_TRUE(deleted->mapped->deleted);
    ASSERT_TRUE(approximate->mapped);
    EXPECT_TRUE(approximate->mapped->approximate);
}

TEST(Forge, watch_surfaces_new_comments_and_pulled_heads) {
    auto server = liquid::test::forge_double{};
    auto client =
        liquid::forge_client{server.url(), {.poll_interval = std::chrono::milliseconds{1}}};
    auto digest = liquid::parse_digest(
        "blake3:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    ASSERT_TRUE(digest);
    auto review = liquid::runtime::block_on(client.create_review(liquid::change_id{*digest}));
    ASSERT_TRUE(review);
    auto watched = liquid::runtime::block_on(client.watch(review->id));
    ASSERT_TRUE(watched);

    static_cast<void>(server.add_comment(
        review->id,
        "new comment",
        {.change = review->change, .path = "README.md", .line = 1, .column = 1}));
    auto comment = liquid::runtime::block_on(watched->next());
    ASSERT_TRUE(comment);
    ASSERT_EQ(comment->kind, liquid::forge_event_kind::comment_added);
    ASSERT_TRUE(comment->comment);
    EXPECT_EQ(comment->comment->body, "new comment");

    auto head = server.advance_head("README.md", "new head\n");
    ASSERT_TRUE(head);
    auto moved = liquid::runtime::block_on(watched->next());
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved->kind, liquid::forge_event_kind::head_moved);
    ASSERT_TRUE(moved->head);
    EXPECT_EQ(liquid::to_string(moved->head->value), liquid::to_string(head->value));
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
TEST(ForgeCli, built_binary_pins_review_json) {
    auto server = liquid::test::forge_double{"cli-token"};
    repository_fixture output;
    auto ignored = std::error_code{};
    std::filesystem::create_directories(output.path(), ignored);
    ASSERT_FALSE(ignored);
    const auto path = output.path() / "review.json";
    const auto change =
        std::string{"blake3:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"};
    const auto open = "LIQUID_TOKEN=cli-token " + shell_quote(LIQUID_CLI) + " review open --lake " +
                      shell_quote(server.url()) + " --json " + shell_quote(change) + " > " +
                      shell_quote(path.string());
    ASSERT_EQ(std::system(open.c_str()), 0);
    EXPECT_EQ(read_file(path),
              "{\"id\":\"review-1\",\"change\":\"" + change +
                  "\",\"state\":\"open\",\"reviewers\":[],\"created\":1788537601,"
                  "\"updated\":1788537601}\n");

    const auto list = "LIQUID_TOKEN=cli-token " + shell_quote(LIQUID_CLI) + " review list --lake " +
                      shell_quote(server.url()) + " --json > " + shell_quote(path.string());
    ASSERT_EQ(std::system(list.c_str()), 0);
    EXPECT_EQ(read_file(path),
              "[{\"id\":\"review-1\",\"change\":\"" + change +
                  "\",\"state\":\"open\",\"reviewers\":[],\"created\":1788537601,"
                  "\"updated\":1788537601}]\n");
}
#endif
