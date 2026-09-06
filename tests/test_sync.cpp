#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"
#include "temporary_directory.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory()
        : path_(liquid::test::fixture_path("liquid-sync-test-" + std::to_string(::getpid()) + "-" +
                                           std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto accepted_verdicts(std::span<const liquid::sync_ref_update> updates)
    -> std::vector<liquid::sync_ref_verdict> {
    auto output = std::vector<liquid::sync_ref_verdict>{};
    output.reserve(updates.size());
    for (const auto& update : updates) {
        output.push_back(liquid::sync_ref_verdict{update.name});
    }
    return output;
}

auto refused_verdicts(std::span<const liquid::sync_ref_update> updates,
                      std::string_view name,
                      std::string reason) -> std::vector<liquid::sync_ref_verdict> {
    auto output = accepted_verdicts(updates);
    const auto found = std::ranges::find(output, name, &liquid::sync_ref_verdict::name);
    if (found != output.end()) {
        found->verdict = liquid::sync_ref_verdict_kind::refused;
        found->reason = std::move(reason);
    }
    return output;
}

auto legacy_metadata_reply(std::string cursor) -> liquid::result<std::vector<std::byte>> {
    namespace cbor = liquid::encoding::cbor;
    auto payload = cbor::encode(cbor::value{cbor::value::map{
        {"v", std::uint64_t{1}},
        {"cursor", std::move(cursor)},
        {"refs", cbor::value{cbor::value::array{}}},
        {"objects", cbor::value{cbor::value::array{}}},
    }});
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto output = std::vector<std::byte>{std::byte{1}};
    const auto size = static_cast<std::uint32_t>(payload->size());
    for (const auto shift : {24U, 16U, 8U, 0U}) {
        output.push_back(std::byte{static_cast<std::uint8_t>(size >> shift)});
    }
    output.insert(output.end(), payload->begin(), payload->end());
    output.push_back(std::byte{255});
    output.insert(output.end(), 4, std::byte{});
    return output;
}

auto content_length(std::string_view header) -> std::size_t {
    const auto label = std::string_view{"Content-Length:"};
    const auto found = header.find(label);
    if (found == std::string_view::npos) {
        return 0;
    }
    auto value = header.substr(found + label.size());
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    value = value.substr(0, value.find("\r\n"));
    auto output = std::size_t{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} ? output : 0;
}

auto send_all(int socket, std::string_view value) -> bool {
    auto sent = std::size_t{};
    while (sent < value.size()) {
        const auto count = ::send(socket, value.data() + sent, value.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

template <class T>
liquid::runtime::task<void>
complete_async(liquid::runtime::task<T> operation,
               std::shared_ptr<std::promise<liquid::result<T>>> completion) {
    try {
        completion->set_value(co_await std::move(operation));
    } catch (...) {
        completion->set_value(std::unexpected(liquid::runtime::detail::exception_error(
            std::current_exception(), "sync test transport")));
    }
    co_return liquid::result<void>{};
}

template <class T> auto wait_async(liquid::runtime::task<T> operation) -> liquid::result<T> {
    auto completion = std::make_shared<std::promise<liquid::result<T>>>();
    auto future = completion->get_future();
    liquid::runtime::spawn(complete_async(std::move(operation), completion), {});
    return future.get();
}

class sync_http_server {
public:
    explicit sync_http_server(liquid::sync_server& server, bool legacy_push_reply = false)
        : server_(&server), legacy_push_reply_(legacy_push_reply) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("server socket failed");
        }
        auto address = sockaddr_in{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            ::close(listener_);
            throw std::runtime_error("server bind failed");
        }
        auto length = socklen_t{sizeof(address)};
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("server address failed");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~sync_http_server() {
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
        if (thread_.joinable()) {
            thread_.join();
        }
        ::close(listener_);
    }

    auto url() const -> std::string {
        return "http://127.0.0.1:" + std::to_string(port_) + "/repository";
    }

private:
    void serve() {
        while (!stopping_.load()) {
            const auto connection = ::accept(listener_, nullptr, nullptr);
            if (connection < 0) {
                continue;
            }
            if (!stopping_.load()) {
                handle(connection);
            }
            ::close(connection);
        }
    }

    void handle(int connection) {
        auto request = std::string{};
        auto expected = std::size_t{};
        for (;;) {
            auto block = std::array<char, 4096>{};
            const auto count = ::recv(connection, block.data(), block.size(), 0);
            if (count <= 0) {
                return;
            }
            request.append(block.data(), static_cast<std::size_t>(count));
            const auto header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                expected = header_end + 4 + content_length(request.substr(0, header_end + 4));
                if (request.size() >= expected) {
                    break;
                }
            }
        }
        const auto line_end = request.find("\r\n");
        const auto first_space = request.find(' ');
        const auto second_space = request.find(' ', first_space + 1);
        if (line_end == std::string::npos || first_space == std::string::npos ||
            second_space == std::string::npos) {
            respond(connection, 400, {});
            return;
        }
        const auto method = std::string_view{request}.substr(0, first_space);
        const auto target =
            std::string_view{request}.substr(first_space + 1, second_space - first_space - 1);
        const auto header_end = request.find("\r\n\r\n");
        const auto body_text = std::string_view{request}.substr(header_end + 4);
        const auto* first = reinterpret_cast<const std::byte*>(body_text.data());
        const auto body = std::span<const std::byte>{first, body_text.size()};
        if (method == "POST" && target.ends_with("/sync/find-missing")) {
            find_missing(connection, body);
        } else if (method == "POST" && target.ends_with("/sync/chunks")) {
            store_chunks(connection, body);
        } else if (method == "POST" && target.ends_with("/sync/push")) {
            push(connection, body);
        } else if (method == "POST" && target.ends_with("/sync/pull")) {
            pull(connection, body);
        } else if (method == "GET" && target.find("/sync/chunks/") != std::string_view::npos) {
            read_range(connection, target);
        } else {
            respond(connection, 404, {});
        }
    }

    void find_missing(int connection, std::span<const std::byte> body) {
        respond(connection, wait_async(server_->handle_find_missing(body)));
    }

    void store_chunks(int connection, std::span<const std::byte> body) {
        auto stored = wait_async(server_->handle_chunks(body));
        if (!stored) {
            respond(connection, stored.error());
            return;
        }
        respond(connection, 200, {});
    }

    void push(int connection, std::span<const std::byte> body) {
        auto pushed = wait_async(server_->handle_push(body));
        if (!pushed || !legacy_push_reply_) {
            respond(connection, std::move(pushed));
            return;
        }
        respond(connection, legacy_metadata_reply("legacy"));
    }

    void pull(int connection, std::span<const std::byte> body) {
        respond(connection, wait_async(server_->handle_pull(body)));
    }

    void read_range(int connection, std::string_view target) {
        const auto marker = target.find("/sync/chunks/") + std::string_view{"/sync/chunks/"}.size();
        const auto query = target.find('?', marker);
        auto id = liquid::parse_digest(target.substr(marker, query - marker));
        if (!id || query == std::string_view::npos) {
            respond(connection, 400, {});
            return;
        }
        const auto offset_label = std::string_view{"?offset="};
        const auto length_label = std::string_view{"&length="};
        const auto length_at = target.find(length_label, query);
        auto offset = std::uint64_t{};
        auto length = std::uint64_t{};
        const auto offset_text =
            target.substr(query + offset_label.size(), length_at - query - offset_label.size());
        const auto length_text = target.substr(length_at + length_label.size());
        const auto offset_result =
            std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset);
        const auto length_result =
            std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
        if (offset_result.ec != std::errc{} || length_result.ec != std::errc{}) {
            respond(connection, 404, {});
            return;
        }
        if (length > std::numeric_limits<std::size_t>::max()) {
            respond(connection, 400, {});
            return;
        }
        auto output = std::vector<std::byte>(static_cast<std::size_t>(length));
        auto read = wait_async(server_->handle_chunks(*id, offset, output));
        if (!read) {
            respond(connection, read.error());
            return;
        }
        output.resize(static_cast<std::size_t>(*read));
        respond(connection, 200, output);
    }

    void respond(int connection, liquid::result<std::vector<std::byte>> body) {
        if (!body) {
            respond(connection, body.error());
            return;
        }
        respond(connection, 200, *body);
    }

    void respond(int connection, const liquid::error& error) {
        auto status = 500U;
        if (error.code == liquid::errc::invalid_argument || error.code == liquid::errc::corrupt) {
            status = 400;
        } else if (error.code == liquid::errc::unauthorized) {
            status = 401;
        } else if (error.code == liquid::errc::not_found) {
            status = 404;
        } else if (error.code == liquid::errc::conflict ||
                   error.code == liquid::errc::already_exists) {
            status = 409;
        }
        auto body = liquid::sync_server::encode_error(error);
        respond(connection,
                status,
                body ? std::span<const std::byte>{*body} : std::span<const std::byte>{});
    }

    void respond(int connection, unsigned status, std::span<const std::byte> body) {
        const auto reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found";
        auto response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        response += "Content-Type: application/vnd.liquid.sync-v1\r\n";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response.append(reinterpret_cast<const char*>(body.data()), body.size());
        static_cast<void>(send_all(connection, response));
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::atomic_bool stopping_{};
    std::thread thread_;
    liquid::sync_server* server_{};
    bool legacy_push_reply_{};
};

auto export_all(liquid::repository& repository) -> liquid::result<std::vector<liquid::sync_item>> {
    auto output = std::vector<liquid::sync_item>{};
    auto cursor = liquid::journal_cursor{};
    for (;;) {
        auto page = std::array<liquid::sync_item, 2>{};
        auto exported = liquid::runtime::block_on(repository.export_drips(cursor, page));
        if (!exported) {
            return std::unexpected(exported.error());
        }
        output.insert(output.end(),
                      std::make_move_iterator(page.begin()),
                      std::make_move_iterator(page.begin() + exported->count));
        cursor = exported->cursor;
        if (exported->done) {
            return output;
        }
    }
}

TEST(Sync, exchanges_disk_repositories_and_only_new_objects) {
    temporary_directory source_path;
    temporary_directory destination_path;
    temporary_directory server_path;
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(server_path.path() / "store"));
    ASSERT_TRUE(opened_store);
    auto server_store = std::move(*opened_store);
    auto server_options = liquid::repository_options{};
    server_options.store = &server_store;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create("repository", server_options));
    ASSERT_TRUE(server_repository);
    auto sync_server = liquid::sync_server{*server_repository, server_store};
    sync_http_server server{sync_server};
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    auto endpoint = liquid::sync_endpoint{server.url()};
    auto first = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first) << first.error().message << " " << first.error().context;
    EXPECT_GT(first->sent.objects, 0U);

    ASSERT_TRUE(liquid::runtime::block_on(source->write("two.txt", bytes("two"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));
    auto second = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second) << second.error().message << " " << second.error().context;
    EXPECT_GT(second->sent.objects, 0U);
    EXPECT_GT(second->sent.bytes, 0U);

    auto unchanged = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->sent.objects, 0U);
    EXPECT_EQ(unchanged->sent.bytes, 0U);

    auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message << " " << pulled.error().context;
    EXPECT_GT(pulled->received.objects, 0U);
    EXPECT_GT(pulled->received.bytes, 0U);
    auto one = liquid::runtime::block_on(destination->read("one.txt"));
    auto two = liquid::runtime::block_on(destination->read("two.txt"));
    ASSERT_TRUE(one);
    ASSERT_TRUE(two);
    EXPECT_EQ(text(*one), "one");
    EXPECT_EQ(text(*two), "two");
}

TEST(Sync, ChangeRefusalProducesAReferenceOutcome) {
    temporary_directory source_path;
    temporary_directory server_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(server_path.path() / "store"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(opened_store);
    auto server_store = std::move(*opened_store);
    auto repository_options = liquid::repository_options{};
    repository_options.store = &server_store;
    auto destination =
        liquid::runtime::block_on(liquid::repository::create("repository", repository_options));
    ASSERT_TRUE(destination);

    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("two.txt", bytes("two"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));

    auto calls = std::size_t{};
    auto authorize = [&calls](const liquid::sync_item& item,
                              std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind != liquid::sync_item_kind::change) {
            co_return accepted_verdicts(updates);
        }
        if (updates.size() != 1) {
            co_return std::unexpected(liquid::error{
                liquid::errc::internal, "change reference context is missing", "test"});
        }
        EXPECT_EQ(updates.front().name, "main");
        ++calls;
        if (calls == 2) {
            co_return refused_verdicts(updates, "main", "change is not authorized");
        }
        co_return accepted_verdicts(updates);
    };
    auto server = liquid::sync_server{
        *destination, server_store, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 1U);
    EXPECT_EQ(pushed->references.front().name, "main");
    EXPECT_EQ(pushed->references.front().outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(pushed->references.front().reason, "change is not authorized");
    EXPECT_EQ(calls, 2U);
    auto root = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->entries.empty());
    auto head = liquid::runtime::block_on(destination->head());
    EXPECT_FALSE(head);
    EXPECT_EQ(head.error().code, liquid::errc::not_found);
}

TEST(Sync, RefusalOfOnlyReferenceLeavesRepositoryUnchanged) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    auto source_head = liquid::runtime::block_on(source->cut("first"));
    ASSERT_TRUE(source_head);

    auto change_calls = std::size_t{};
    auto reference_calls = std::size_t{};
    auto authorize = [&](const liquid::sync_item& item,
                         std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind == liquid::sync_item_kind::change) {
            ++change_calls;
            if (updates.size() != 1) {
                co_return std::unexpected(liquid::error{
                    liquid::errc::internal, "change reference context is missing", "test"});
            }
            EXPECT_EQ(updates.front().name, "main");
            co_return accepted_verdicts(updates);
        }
        EXPECT_EQ(item.kind, liquid::sync_item_kind::reference);
        if (updates.size() != 1) {
            co_return std::unexpected(liquid::error{
                liquid::errc::internal, "reference update context is missing", "test"});
        }
        ++reference_calls;
        EXPECT_EQ(updates.front().name, "main");
        EXPECT_FALSE(updates.front().old_head);
        EXPECT_EQ(liquid::to_string(updates.front().new_head.value),
                  liquid::to_string(source_head->value));
        EXPECT_TRUE(updates.front().fast_forward);
        co_return refused_verdicts(updates, "main", "reference is not authorized");
    };
    auto server =
        liquid::sync_server{*destination, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 1U);
    EXPECT_EQ(pushed->references.front().outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(pushed->references.front().reason, "reference is not authorized");
    EXPECT_EQ(change_calls, 1U);
    EXPECT_EQ(reference_calls, 1U);
    auto root = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->entries.empty());
    auto destination_head = liquid::runtime::block_on(destination->head());
    EXPECT_FALSE(destination_head);
    EXPECT_EQ(destination_head.error().code, liquid::errc::not_found);
}

TEST(Sync, MixedReferenceOutcomesApplyAcceptedReferenceAndPreserveReason) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    auto source_head = liquid::runtime::block_on(source->cut("first"));
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("feature", *source_head)));

    const auto reason = std::string{"protected: deploy approval required"};
    auto authorize = [&reason](const liquid::sync_item& item,
                               std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind == liquid::sync_item_kind::reference) {
            co_return refused_verdicts(updates, "main", reason);
        }
        co_return accepted_verdicts(updates);
    };
    auto server =
        liquid::sync_server{*destination, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 2U);
    const auto applied =
        std::ranges::find(pushed->references, "feature", &liquid::sync_ref_outcome::name);
    const auto refused =
        std::ranges::find(pushed->references, "main", &liquid::sync_ref_outcome::name);
    ASSERT_NE(applied, pushed->references.end());
    ASSERT_NE(refused, pushed->references.end());
    EXPECT_EQ(applied->outcome, liquid::sync_ref_outcome_kind::applied);
    EXPECT_EQ(refused->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(refused->reason, reason);

    auto destination_refs = liquid::runtime::block_on(destination->refs());
    ASSERT_TRUE(destination_refs);
    ASSERT_EQ(destination_refs->size(), 1U);
    EXPECT_EQ(destination_refs->front().name, "feature");
    EXPECT_EQ(liquid::to_string(destination_refs->front().id.value),
              liquid::to_string(source_head->value));
}

TEST(Sync, AtomicPushRefusesEveryReferenceWhenOneIsRefused) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    auto source_head = liquid::runtime::block_on(source->cut("first"));
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("feature", *source_head)));

    auto authorize = [](const liquid::sync_item& item,
                        std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind == liquid::sync_item_kind::reference) {
            co_return refused_verdicts(updates, "main", "main is protected");
        }
        co_return accepted_verdicts(updates);
    };
    auto server =
        liquid::sync_server{*destination, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto options = liquid::sync_endpoint_options{};
    options.atomic = true;
    auto endpoint = liquid::sync_endpoint{http.url(), options};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(pushed->references, [](const auto& outcome) {
        return outcome.outcome == liquid::sync_ref_outcome_kind::refused;
    }));
    const auto main =
        std::ranges::find(pushed->references, "main", &liquid::sync_ref_outcome::name);
    const auto feature =
        std::ranges::find(pushed->references, "feature", &liquid::sync_ref_outcome::name);
    ASSERT_NE(main, pushed->references.end());
    ASSERT_NE(feature, pushed->references.end());
    EXPECT_EQ(main->reason, "main is protected");
    EXPECT_EQ(feature->reason, "atomic push refused because another reference was refused");
    auto destination_refs = liquid::runtime::block_on(destination->refs());
    ASSERT_TRUE(destination_refs);
    EXPECT_TRUE(destination_refs->empty());
    auto root = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->entries.empty());
}

TEST(Sync, ThrowingAuthorizerRefusesEveryReferenceAndKeepsObjects) {
    temporary_directory source_path;
    temporary_directory server_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(server_path.path() / "store"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(opened_store);
    auto server_store = std::move(*opened_store);
    auto repository_options = liquid::repository_options{};
    repository_options.store = &server_store;
    auto destination =
        liquid::runtime::block_on(liquid::repository::create("repository", repository_options));
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    auto source_head = liquid::runtime::block_on(source->cut("first"));
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("feature", *source_head)));

    auto authorize = [](const liquid::sync_item&, std::span<const liquid::sync_ref_update>)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        throw std::runtime_error("policy service unavailable: retry later");
        co_return std::vector<liquid::sync_ref_verdict>{};
    };
    auto server = liquid::sync_server{
        *destination, server_store, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 2U);
    for (const auto& outcome : pushed->references) {
        EXPECT_EQ(outcome.outcome, liquid::sync_ref_outcome_kind::refused);
        EXPECT_EQ(outcome.reason, "policy service unavailable: retry later");
    }
    auto destination_refs = liquid::runtime::block_on(destination->refs());
    ASSERT_TRUE(destination_refs);
    EXPECT_TRUE(destination_refs->empty());
    auto stored = liquid::runtime::block_on(server_store.contains(source_head->value));
    ASSERT_TRUE(stored);
    EXPECT_TRUE(*stored);
}

TEST(Sync, LegacySuccessfulReplyReportsEveryRequestedReferenceAsApplied) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));
    auto server = liquid::sync_server{*destination};
    sync_http_server http{server, true};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    EXPECT_EQ(pushed->cursor.token, "legacy");
    ASSERT_EQ(pushed->references.size(), 1U);
    EXPECT_EQ(pushed->references.front().name, "main");
    EXPECT_EQ(pushed->references.front().outcome, liquid::sync_ref_outcome_kind::applied);
}

TEST(Sync, DivergentReferenceUpdateIsNotFastForward) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", bytes("base"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    {
        auto server = liquid::sync_server{*destination};
        sync_http_server http{server};
        auto endpoint = liquid::sync_endpoint{http.url()};
        ASSERT_TRUE(liquid::runtime::block_on(endpoint.push(*source)));
    }

    ASSERT_TRUE(liquid::runtime::block_on(destination->write("server.txt", bytes("server"))));
    auto old_head = liquid::runtime::block_on(destination->cut("server"));
    ASSERT_TRUE(old_head);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("client.txt", bytes("client"))));
    auto new_head = liquid::runtime::block_on(source->cut("client"));
    ASSERT_TRUE(new_head);

    auto observed = std::optional<liquid::sync_ref_update>{};
    auto authorize = [&observed](const liquid::sync_item& item,
                                 std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind != liquid::sync_item_kind::reference) {
            co_return accepted_verdicts(updates);
        }
        if (updates.size() != 1) {
            co_return std::unexpected(liquid::error{
                liquid::errc::internal, "reference update context is missing", "test"});
        }
        observed = updates.front();
        co_return refused_verdicts(updates, "main", "reference is not authorized");
    };
    auto server =
        liquid::sync_server{*destination, liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed);
    ASSERT_EQ(pushed->references.size(), 1U);
    EXPECT_EQ(pushed->references.front().outcome, liquid::sync_ref_outcome_kind::refused);
    ASSERT_TRUE(observed);
    ASSERT_TRUE(observed->old_head);
    EXPECT_EQ(observed->name, "main");
    EXPECT_EQ(liquid::to_string(observed->old_head->value), liquid::to_string(old_head->value));
    EXPECT_EQ(liquid::to_string(observed->new_head.value), liquid::to_string(new_head->value));
    EXPECT_FALSE(observed->fast_forward);
    auto unchanged_head = liquid::runtime::block_on(destination->head());
    ASSERT_TRUE(unchanged_head);
    EXPECT_EQ(liquid::to_string(unchanged_head->value), liquid::to_string(old_head->value));
}

TEST(Sync, bad_signature_rejects_the_complete_batch) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));
    auto items = export_all(*source);
    ASSERT_TRUE(items);
    const auto drip = std::ranges::find_if(*items, [](const liquid::sync_item& item) {
        return item.kind == liquid::sync_item_kind::drip;
    });
    ASSERT_NE(drip, items->end());
    ASSERT_FALSE(drip->signature.empty());
    auto corrupted = std::move(*drip);
    items->erase(drip);
    corrupted.signature.back() ^= std::byte{1};
    items->push_back(std::move(corrupted));

    auto imported = liquid::runtime::block_on(destination->import_drips(*items));
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, liquid::errc::unauthorized);
    EXPECT_NE(imported.error().context.find("item="), std::string::npos);
    auto root = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->entries.empty());
    auto head = liquid::runtime::block_on(destination->head());
    EXPECT_FALSE(head);
    EXPECT_EQ(head.error().code, liquid::errc::not_found);
}

#if defined(LIQUID_CLI)
auto shell_quote(const std::filesystem::path& value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value.string()) {
        output += character == '\'' ? "'\\''" : std::string{character};
    }
    output += '\'';
    return output;
}

auto available_port() -> std::uint16_t {
    const auto listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return 0;
    }
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(listener);
        return 0;
    }
    auto length = socklen_t{sizeof(address)};
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(listener);
        return 0;
    }
    ::close(listener);
    return ntohs(address.sin_port);
}

class cli_server_process {
public:
    cli_server_process(const std::filesystem::path& repository,
                       std::uint16_t port,
                       std::optional<std::string> allowed_ref = std::nullopt) {
        const auto port_text = std::to_string(port);
        child_ = ::fork();
        if (child_ == 0) {
            if (allowed_ref) {
                ::execl(LIQUID_CLI,
                        LIQUID_CLI,
                        "serve",
                        "sync",
                        repository.c_str(),
                        "--port",
                        port_text.c_str(),
                        "--allow-ref",
                        allowed_ref->c_str(),
                        static_cast<char*>(nullptr));
                std::_Exit(127);
            }
            ::execl(LIQUID_CLI,
                    LIQUID_CLI,
                    "serve",
                    "sync",
                    repository.c_str(),
                    "--port",
                    port_text.c_str(),
                    static_cast<char*>(nullptr));
            std::_Exit(127);
        }
    }

    ~cli_server_process() {
        if (child_ <= 0) {
            return;
        }
        static_cast<void>(::kill(child_, SIGTERM));
        static_cast<void>(::waitpid(child_, nullptr, 0));
    }

    auto ready(std::uint16_t port) -> bool {
        for (auto attempt = 0; attempt < 200; ++attempt) {
            auto status = 0;
            if (::waitpid(child_, &status, WNOHANG) == child_) {
                child_ = -1;
                return false;
            }
            const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
            if (socket >= 0) {
                auto address = sockaddr_in{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port);
                if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
                    0) {
                    static_cast<void>(send_all(socket,
                                               "GET /ready HTTP/1.1\r\n"
                                               "Host: 127.0.0.1\r\n"
                                               "Connection: close\r\n\r\n"));
                    ::close(socket);
                    return true;
                }
                ::close(socket);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

private:
    pid_t child_{-1};
};

TEST(Sync, cli_push_and_pull_round_trip) {
    temporary_directory source_path;
    temporary_directory destination_path;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("cli.txt", bytes("round trip"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("cli")));
    *source = liquid::repository{};
    *destination = liquid::repository{};
    *server_repository = liquid::repository{};
    const auto port = available_port();
    ASSERT_NE(port, 0);
    auto server = cli_server_process{server_path.path(), port, "main"};
    ASSERT_TRUE(server.ready(port));
    const auto url = "http://127.0.0.1:" + std::to_string(port);
    const auto executable = shell_quote(LIQUID_CLI);
    const auto push_output = source_path.path() / "push-output.txt";
    const auto atomic_output = source_path.path() / "atomic-output.txt";
    const auto push = "cd " + shell_quote(source_path.path()) + " && " + executable + " push '" +
                      url + "' > " + shell_quote(push_output);
    const auto atomic_push = "cd " + shell_quote(source_path.path()) + " && " + executable +
                             " push --atomic '" + url + "' > " + shell_quote(atomic_output);
    const auto pull =
        "cd " + shell_quote(destination_path.path()) + " && " + executable + " pull '" + url + "'";
    ASSERT_EQ(std::system(push.c_str()), 0);
    auto reopened_source = liquid::runtime::block_on(liquid::repository::open(source_path.path()));
    ASSERT_TRUE(reopened_source);
    auto source_head = liquid::runtime::block_on(reopened_source->head());
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(liquid::runtime::block_on(reopened_source->set_ref("blocked", *source_head)));
    *reopened_source = liquid::repository{};
    EXPECT_NE(std::system(push.c_str()), 0);
    auto output = std::ifstream{push_output};
    const auto output_text = std::string{std::istreambuf_iterator<char>{output}, {}};
    EXPECT_NE(output_text.find("reference=main outcome=applied"), std::string::npos);
    EXPECT_NE(output_text.find("reference=blocked outcome=refused reason=reference is not allowed"),
              std::string::npos);
    EXPECT_NE(std::system(atomic_push.c_str()), 0);
    auto atomic = std::ifstream{atomic_output};
    const auto atomic_text = std::string{std::istreambuf_iterator<char>{atomic}, {}};
    EXPECT_NE(atomic_text.find("reference=main outcome=refused"), std::string::npos);
    EXPECT_NE(atomic_text.find("reference=blocked outcome=refused reason=reference is not allowed"),
              std::string::npos);
    ASSERT_EQ(std::system(pull.c_str()), 0);
    auto opened = liquid::runtime::block_on(liquid::repository::open(destination_path.path()));
    ASSERT_TRUE(opened);
    auto content = liquid::runtime::block_on(opened->read("cli.txt"));
    ASSERT_TRUE(content) << content.error().message << " " << content.error().context;
    EXPECT_EQ(text(*content), "round trip");
}
#endif

} // namespace
