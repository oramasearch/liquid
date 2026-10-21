// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include <map>
#include <memory>
#include <optional>

#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "build/repository_access.hpp"

#include "drive/source.hpp"
#include "encoding/cbor.hpp"
#include "liquid/attestation.hpp"
#include "liquid/drive.hpp"
#include "liquid/identity.hpp"

#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"
#include "net/retry_after.hpp"
#include "objects/records.hpp"
#include "repository/drive_access.hpp"
#include "repository/store_access.hpp"
#include "temporary_directory.hpp"
#include "vfs/repository_access.hpp"

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

class counting_store_source final : public liquid::remote_blob_source {
public:
    explicit counting_store_source(liquid::store& source) : source_(&source) {}

    auto requests() const -> const std::vector<liquid::digest>& {
        return requests_;
    }

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest>) override {
        co_return std::vector<liquid::digest>{};
    }

    liquid::runtime::task<std::uint64_t> read_range(std::string,
                                                    liquid::digest blob,
                                                    std::uint64_t offset,
                                                    std::span<std::byte> output) override {
        requests_.push_back(blob);
        co_return co_await source_->read(blob, offset, output);
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest, std::vector<std::byte>) override {
        co_return std::unexpected(
            liquid::error{liquid::errc::unsupported, "counting source is read only"});
    }

private:
    liquid::store* source_{};
    std::vector<liquid::digest> requests_;
};

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
    explicit sync_http_server(liquid::sync_server& server,
                              bool legacy_push_reply = false,
                              std::string required_bearer = {},
                              bool repository_available = true)
        : server_(&server), legacy_push_reply_(legacy_push_reply),
          required_bearer_(std::move(required_bearer)),
          repository_available_(repository_available) {
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

    void fail_next_chunk_read() {
        fail_chunk_read_.store(true);
    }

    // Answer the next `count` requests of `route` (find-missing, chunk-upload, push, pull,
    // chunk-read) with `status` and a canonical `unavailable` error body, plus a `Retry-After`
    // header when `retry_after` is not empty, before serving that route normally again.
    void refuse_next(std::string route, unsigned count, unsigned status, std::string retry_after) {
        auto lock = std::scoped_lock{mutex_};
        refusals_[std::move(route)] = refusal{count, status, std::move(retry_after)};
    }

    // How many requests of `route` arrived, refused ones included.
    auto requests(const std::string& route) const -> unsigned {
        auto lock = std::scoped_lock{mutex_};
        const auto found = requests_.find(route);
        return found == requests_.end() ? 0U : found->second;
    }

    auto pending_refusals(const std::string& route) const -> unsigned {
        auto lock = std::scoped_lock{mutex_};
        const auto found = refusals_.find(route);
        return found == refusals_.end() ? 0U : found->second.remaining;
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
        if (refused(connection, route(method, target))) {
            return;
        }
        if (!required_bearer_.empty()) {
            const auto authorization = "\r\nAuthorization: Bearer " + required_bearer_ + "\r\n";
            if (request.find(authorization) == std::string::npos) {
                respond(connection,
                        liquid::error{liquid::errc::unauthorized,
                                      "repository authentication required",
                                      "Authorization"});
                return;
            }
        }
        if (!repository_available_) {
            respond(connection,
                    liquid::error{
                        liquid::errc::not_found, "repository does not exist", std::string{target}});
            return;
        }
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

    static auto route(std::string_view method, std::string_view target) -> std::string {
        if (method == "POST" && target.ends_with("/sync/find-missing")) {
            return "find-missing";
        }
        if (method == "POST" && target.ends_with("/sync/chunks")) {
            return "chunk-upload";
        }
        if (method == "POST" && target.ends_with("/sync/push")) {
            return "push";
        }
        if (method == "POST" && target.ends_with("/sync/pull")) {
            return "pull";
        }
        if (method == "GET" && target.find("/sync/chunks/") != std::string_view::npos) {
            return "chunk-read";
        }
        return "other";
    }

    auto refused(int connection, const std::string& name) -> bool {
        auto status = 0U;
        auto retry_after = std::string{};
        {
            auto lock = std::scoped_lock{mutex_};
            ++requests_[name];
            const auto found = refusals_.find(name);
            if (found == refusals_.end() || found->second.remaining == 0) {
                return false;
            }
            --found->second.remaining;
            status = found->second.status;
            retry_after = found->second.retry_after;
        }
        auto body = liquid::sync_server::encode_error(liquid::error{
            liquid::errc::unavailable, "repository store is being moved", "store_unavailable"});
        auto headers = std::string{};
        if (!retry_after.empty()) {
            headers = "Retry-After: " + retry_after + "\r\n";
        }
        respond(connection,
                status,
                body ? std::span<const std::byte>{*body} : std::span<const std::byte>{},
                headers);
        return true;
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
        if (fail_chunk_read_.exchange(false)) {
            return;
        }
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
        } else if (error.code == liquid::errc::busy) {
            status = 429;
        }
        auto body = liquid::sync_server::encode_error(error);
        respond(connection,
                status,
                body ? std::span<const std::byte>{*body} : std::span<const std::byte>{});
    }

    void respond(int connection,
                 unsigned status,
                 std::span<const std::byte> body,
                 std::string_view extra_headers = {}) {
        const auto reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found";
        auto response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        response += "Content-Type: application/vnd.liquid.sync-v1\r\n";
        response += extra_headers;
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response.append(reinterpret_cast<const char*>(body.data()), body.size());
        static_cast<void>(send_all(connection, response));
    }

    struct refusal {
        unsigned remaining{};
        unsigned status{};
        std::string retry_after;
    };

    int listener_{-1};
    std::uint16_t port_{};
    std::atomic_bool stopping_{};
    std::atomic_bool fail_chunk_read_{};
    mutable std::mutex mutex_;
    std::map<std::string, refusal> refusals_;
    std::map<std::string, unsigned> requests_;
    std::thread thread_;
    liquid::sync_server* server_{};
    bool legacy_push_reply_{};
    std::string required_bearer_;
    bool repository_available_{true};
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

TEST(Sync, ReceiveMetadataRegistersBlobsWithoutFetchingChunks) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source_store =
        liquid::runtime::block_on(liquid::store::open(source_path.path() / "store"));
    ASSERT_TRUE(source_store);
    auto source_options = liquid::repository_options{};
    source_options.store = &*source_store;
    auto source =
        liquid::runtime::block_on(liquid::repository::create("image-source", source_options));
    ASSERT_TRUE(source);
    auto content = std::vector<std::byte>(512U * 1024U);
    for (auto index = std::size_t{}; index < content.size(); ++index) {
        content[index] = std::byte{static_cast<unsigned char>((index * 31U) & 0xffU)};
    }
    auto content_id = liquid::runtime::block_on(source_store->put(content));
    ASSERT_TRUE(content_id);
    auto source_blob =
        liquid::repository_detail::store_access::blob_for(*source_store, *content_id);
    ASSERT_TRUE(source_blob);
    source_blob->remote = liquid::remote_blob_location{"source-url", "image-instance"};
    ASSERT_TRUE(liquid::runtime::block_on(source->write_remote("image.bin", *source_blob)));
    auto root = liquid::runtime::block_on(source->root_digest());
    ASSERT_TRUE(root);
    auto tree_bytes = liquid::runtime::block_on(source_store->get(*root));
    ASSERT_TRUE(tree_bytes);
    auto tree = liquid::objects::decode_tree(*tree_bytes);
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->entries.size(), 1U);
    const auto blob_record = tree->entries.front().ref;
    auto blob_bytes = liquid::runtime::block_on(source_store->get(blob_record));
    ASSERT_TRUE(blob_bytes);
    auto blob = liquid::objects::decode_blob(*blob_bytes);
    ASSERT_TRUE(blob);

    auto remote = std::make_shared<counting_store_source>(*source_store);
    auto destination_options = liquid::store_options{};
    destination_options.remote_sources.emplace("source-url", remote);
    auto destination = liquid::runtime::block_on(
        liquid::store::open(destination_path.path() / "store", destination_options));
    ASSERT_TRUE(destination);
    auto session = liquid::sync_session{*destination, *remote, "source-url", "image-instance"};
    const auto roots = std::array<liquid::digest, 1>{*root};
    auto report = liquid::runtime::block_on(session.receive_metadata(roots));
    ASSERT_TRUE(report) << report.error().message << " (" << report.error().context << ")";
    EXPECT_EQ(report->trees, 1U);
    EXPECT_EQ(report->blobs, 1U);
    EXPECT_EQ(report->bytes_remote, blob->size);

    auto availability = liquid::runtime::block_on(destination->has(blob->content));
    ASSERT_TRUE(availability);
    EXPECT_EQ(*availability, liquid::blob_availability::remote);
    for (const auto& chunk : blob->chunks) {
        auto present = liquid::runtime::block_on(destination->contains(chunk));
        ASSERT_TRUE(present);
        EXPECT_FALSE(*present);
    }
    ASSERT_EQ(remote->requests().size(), 2U);
    EXPECT_EQ(liquid::to_string(remote->requests()[0]), liquid::to_string(*root));
    EXPECT_EQ(liquid::to_string(remote->requests()[1]), liquid::to_string(blob_record));
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

TEST(Sync, push_and_pull_clone_preserve_unresolved_conflicts) {
    temporary_directory source_path;
    temporary_directory clone_path;
    temporary_directory server_path;
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(server_path.path() / "store"));
    ASSERT_TRUE(opened_store);
    auto server_store = std::move(*opened_store);
    auto server_options = liquid::repository_options{};
    server_options.store = &server_store;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create("repository", server_options));
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(server_repository);
    ASSERT_TRUE(source);

    ASSERT_TRUE(liquid::runtime::block_on(source->write("document.txt", bytes("base"))));
    auto base_tree = liquid::runtime::block_on(source->root());
    ASSERT_TRUE(base_tree);
    ASSERT_EQ(base_tree->entries.size(), 1U);
    const auto base = base_tree->entries.front().ref;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("document.txt", bytes("left"))));
    auto left_tree = liquid::runtime::block_on(source->root());
    ASSERT_TRUE(left_tree);
    ASSERT_EQ(left_tree->entries.size(), 1U);
    const auto left = left_tree->entries.front().ref;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("document.txt", bytes("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("safe.txt", bytes("available"))));
    auto right_tree = liquid::runtime::block_on(source->root());
    ASSERT_TRUE(right_tree);
    const auto right_entry =
        std::ranges::find(right_tree->entries, "document.txt", &liquid::tree_entry::name);
    ASSERT_NE(right_entry, right_tree->entries.end());
    const auto right = right_entry->ref;

    auto conflict = liquid::conflict{
        .base = base,
        .left = left,
        .right = right,
        .region = {.path = "document.txt", .bytes = liquid::conflict_byte_range{0, 5}},
        .kind = liquid::conflict_kind::text,
        .explanation = "both replicas changed the same bytes"};
    auto conflict_id = liquid::runtime::block_on(source->record_conflict(std::move(conflict)));
    ASSERT_TRUE(conflict_id);
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("record unresolved conflict")));

    auto sync_server = liquid::sync_server{*server_repository, server_store};
    sync_http_server http{sync_server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message << " " << pushed.error().context;
    auto server_conflict =
        liquid::runtime::block_on(server_repository->conflict_at("document.txt"));
    ASSERT_TRUE(server_conflict) << server_conflict.error().message;
    EXPECT_EQ(liquid::to_string(server_conflict->id), liquid::to_string(*conflict_id));

    auto clone = liquid::runtime::block_on(liquid::repository::create(clone_path.path()));
    ASSERT_TRUE(clone);
    auto pulled = liquid::runtime::block_on(endpoint.pull(*clone));
    ASSERT_TRUE(pulled) << pulled.error().message << " " << pulled.error().context;
    auto cloned_conflict = liquid::runtime::block_on(clone->conflict_at("document.txt"));
    ASSERT_TRUE(cloned_conflict) << cloned_conflict.error().message;
    EXPECT_EQ(liquid::to_string(cloned_conflict->id), liquid::to_string(*conflict_id));
    auto safe = liquid::runtime::block_on(clone->read("safe.txt"));
    ASSERT_TRUE(safe);
    EXPECT_EQ(text(*safe), "available");
}

TEST(Sync, peer_bucket_returns_retry_report_and_leaves_repository_unchanged) {
    temporary_directory source_path;
    temporary_directory destination_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto options = liquid::repository_options{};
    options.flow.per_actor = {.rate_per_s = 1, .burst = 1};
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path(), options));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    auto server = liquid::sync_server{*destination};
    sync_http_server http{server};
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << (pushed ? "" : pushed.error().message);
    EXPECT_GT(pushed->retry_after_ms, 0U);
    EXPECT_EQ(pushed->exhausted_scope, "actor");
    auto root = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->entries.empty());
    auto head = liquid::runtime::block_on(destination->head());
    EXPECT_FALSE(head);
    EXPECT_EQ(head.error().code, liquid::errc::not_found);
}

TEST(RetryAfter, ReadsDelaySecondsAndEveryHttpDateForm) {
    using namespace std::chrono;
    // 1994-11-06 08:49:37 UTC, the example instant RFC 9110 uses for all three date forms.
    const auto instant = sys_days{year{1994} / November / 6} + hours{8} + minutes{49} + seconds{37};
    const auto before = instant - seconds{5};
    EXPECT_EQ(liquid::net_detail::parse_retry_after("0", before), milliseconds{0});
    EXPECT_EQ(liquid::net_detail::parse_retry_after("1", before), milliseconds{1000});
    EXPECT_EQ(liquid::net_detail::parse_retry_after(" 120 ", before), milliseconds{120000});
    EXPECT_EQ(liquid::net_detail::parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", before),
              milliseconds{5000});
    EXPECT_EQ(liquid::net_detail::parse_retry_after("Sunday, 06-Nov-94 08:49:37 GMT", before),
              milliseconds{5000});
    EXPECT_EQ(liquid::net_detail::parse_retry_after("Sun Nov  6 08:49:37 1994", before),
              milliseconds{5000});
    // A date that has passed asks for no wait.
    EXPECT_EQ(
        liquid::net_detail::parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", instant + hours{1}),
        milliseconds{0});
    // An enormous delay is clamped rather than overflowing; it is still far past any budget.
    const auto huge = liquid::net_detail::parse_retry_after("99999999999999999999999999", before);
    ASSERT_TRUE(huge);
    EXPECT_GT(*huge, hours{24 * 365});
    for (const auto* malformed : {"",
                                  "  ",
                                  "-1",
                                  "1.5",
                                  "soon",
                                  "Sun, 06 Nov 1994 08:49:37 UTC",
                                  "Sun, 31 Feb 1994 08:49:37 GMT",
                                  "Sun, 06 Nov 1994 25:49:37 GMT",
                                  "Sun Nov 6 08:49:37 1994"}) {
        EXPECT_FALSE(liquid::net_detail::parse_retry_after(malformed, before)) << malformed;
    }
}

namespace {

// A server repository behind the fake HTTP adapter, and a local source repository with one cut.
struct retry_fixture {
    temporary_directory source_path;
    temporary_directory destination_path;
    temporary_directory server_path;
    liquid::store server_store;
    liquid::repository server_repository;
    liquid::repository source;
    liquid::repository destination;
    std::unique_ptr<liquid::sync_server> sync_server;
    std::unique_ptr<sync_http_server> http;

    retry_fixture()
        : server_store(open_store(server_path.path() / "store")),
          server_repository(create_repository("repository", server_store)),
          source(create_local(source_path.path())),
          destination(create_local(destination_path.path())) {
        sync_server = std::make_unique<liquid::sync_server>(server_repository, server_store);
        http = std::make_unique<sync_http_server>(*sync_server);
        expect(liquid::runtime::block_on(source.write("one.txt", bytes("one"))));
        expect(liquid::runtime::block_on(source.cut("first")));
    }

    static auto open_store(const std::filesystem::path& path) -> liquid::store {
        auto opened = liquid::runtime::block_on(liquid::store::open(path));
        expect(opened);
        return std::move(*opened);
    }

    static auto create_repository(std::string name, liquid::store& store) -> liquid::repository {
        auto options = liquid::repository_options{};
        options.store = &store;
        auto created = liquid::runtime::block_on(liquid::repository::create(name, options));
        expect(created);
        return std::move(*created);
    }

    static auto create_local(const std::filesystem::path& path) -> liquid::repository {
        auto created = liquid::runtime::block_on(liquid::repository::create(path));
        expect(created);
        return std::move(*created);
    }

    template <class T> static void expect(const liquid::result<T>& value) {
        if (!value) {
            throw std::runtime_error(value.error().message + " " + value.error().context);
        }
    }
};

} // namespace

TEST(Sync, RetriesEveryRequestRefused503WithRetryAfter) {
    retry_fixture fixture;
    auto& http = *fixture.http;
    for (const auto* route : {"find-missing", "chunk-upload", "push"}) {
        http.refuse_next(route, 1, 503, "0");
    }
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto pushed = liquid::runtime::block_on(endpoint.push(fixture.source));
    ASSERT_TRUE(pushed) << pushed.error().message << " " << pushed.error().context;
    EXPECT_GT(pushed->sent.objects, 0U);
    for (const auto* route : {"find-missing", "chunk-upload", "push"}) {
        EXPECT_EQ(http.pending_refusals(route), 0U) << route;
    }
    EXPECT_EQ(http.requests("find-missing"), 2U);
    EXPECT_EQ(http.requests("push"), 2U);
    EXPECT_GE(http.requests("chunk-upload"), 2U);

    for (const auto* route : {"pull", "chunk-read"}) {
        http.refuse_next(route, 1, 503, "0");
    }
    auto pulled = liquid::runtime::block_on(endpoint.pull(fixture.destination));
    ASSERT_TRUE(pulled) << pulled.error().message << " " << pulled.error().context;
    EXPECT_EQ(http.requests("pull"), 2U);
    EXPECT_EQ(http.pending_refusals("chunk-read"), 0U);
    EXPECT_GE(http.requests("chunk-read"), 2U);
    auto one = liquid::runtime::block_on(fixture.destination.read("one.txt"));
    ASSERT_TRUE(one);
    EXPECT_EQ(text(*one), "one");
}

TEST(Sync, RetryWaitsForRetryAfterSecondsAndHonoursAnHttpDate) {
    retry_fixture fixture;
    auto& http = *fixture.http;
    auto endpoint = liquid::sync_endpoint{http.url()};

    http.refuse_next("push", 1, 503, "1");
    const auto started = std::chrono::steady_clock::now();
    auto pushed = liquid::runtime::block_on(endpoint.push(fixture.source));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(pushed) << pushed.error().message << " " << pushed.error().context;
    EXPECT_EQ(http.requests("push"), 2U);
    EXPECT_GE(elapsed, std::chrono::milliseconds{950});

    // An HTTP-date that has already passed is a retry with no wait.
    http.refuse_next("pull", 1, 503, "Sun, 06 Nov 1994 08:49:37 GMT");
    auto pulled = liquid::runtime::block_on(endpoint.pull(fixture.destination));
    ASSERT_TRUE(pulled) << pulled.error().message << " " << pulled.error().context;
    EXPECT_EQ(http.requests("pull"), 2U);

    // A 429 that carries Retry-After is retried the same way.
    http.refuse_next("find-missing", 2, 429, "0");
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("two.txt", bytes("two"))));
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.cut("second")));
    auto again = liquid::runtime::block_on(endpoint.push(fixture.source));
    ASSERT_TRUE(again) << again.error().message << " " << again.error().context;
    EXPECT_EQ(http.requests("find-missing"), 3U + 1U);
    EXPECT_EQ(http.pending_refusals("find-missing"), 0U);
}

TEST(Sync, RefusalsWithoutRetryAfterOrOfOtherStatusesAreNotRetried) {
    retry_fixture fixture;
    auto& http = *fixture.http;
    auto endpoint = liquid::sync_endpoint{http.url()};

    http.refuse_next("find-missing", 1, 503, "");
    auto bare = liquid::runtime::block_on(endpoint.push(fixture.source));
    ASSERT_FALSE(bare);
    EXPECT_EQ(bare.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 1U);

    http.refuse_next("find-missing", 1, 500, "0");
    auto server_error = liquid::runtime::block_on(endpoint.push(fixture.source));
    ASSERT_FALSE(server_error);
    EXPECT_EQ(server_error.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 2U);

    http.refuse_next("find-missing", 1, 503, "whenever");
    auto malformed = liquid::runtime::block_on(endpoint.push(fixture.source));
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 3U);
}

TEST(Sync, RetryStopsWhenAttemptsOrTheWaitBudgetAreSpent) {
    retry_fixture fixture;
    auto& http = *fixture.http;

    auto options = liquid::sync_endpoint_options{};
    options.retry_attempts = 3;
    auto limited = liquid::sync_endpoint{http.url(), options};
    http.refuse_next("find-missing", 100, 503, "0");
    auto exhausted = liquid::runtime::block_on(limited.push(fixture.source));
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 3U);

    // A wait longer than the remaining budget is not taken: the refusal is returned at once.
    auto endpoint = liquid::sync_endpoint{http.url()};
    http.refuse_next("find-missing", 1, 503, "120");
    const auto started = std::chrono::steady_clock::now();
    auto too_long = liquid::runtime::block_on(endpoint.push(fixture.source));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 4U);
    EXPECT_LT(elapsed, std::chrono::seconds{10});

    // The budget is a total across retries: two 1 s waits do not fit in 1.5 s, so the second
    // refusal is returned after one wait.
    auto budget = liquid::sync_endpoint_options{};
    budget.retry_budget = std::chrono::milliseconds{1500};
    auto budgeted = liquid::sync_endpoint{http.url(), budget};
    http.refuse_next("find-missing", 5, 503, "1");
    auto spent = liquid::runtime::block_on(budgeted.push(fixture.source));
    ASSERT_FALSE(spent);
    EXPECT_EQ(spent.error().code, liquid::errc::unavailable);
    EXPECT_EQ(http.requests("find-missing"), 6U);
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

class scoped_liquid_home {
public:
    scoped_liquid_home() {
        if (const auto* value = std::getenv("LIQUID_HOME")) {
            previous_ = value;
        }
    }

    ~scoped_liquid_home() {
        if (previous_) {
            setenv("LIQUID_HOME", previous_->c_str(), 1);
        } else {
            unsetenv("LIQUID_HOME");
        }
    }

    scoped_liquid_home(const scoped_liquid_home&) = delete;
    scoped_liquid_home& operator=(const scoped_liquid_home&) = delete;

    static auto select(const std::filesystem::path& path) -> void {
        setenv("LIQUID_HOME", path.c_str(), 1);
    }

private:
    std::optional<std::string> previous_;
};

auto open_local_signer() -> liquid::signer {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    return std::move(*signer);
}

// Re-signs an exported drip with `signing_key` while its body and frame name `claimed`.
auto forge_drip(liquid::sync_item drip, liquid::signer& signing_key, const liquid::actor& claimed)
    -> liquid::sync_item {
    auto body = liquid::encoding::cbor::decode(drip.body);
    EXPECT_TRUE(body) << body.error().message;
    auto* fields = std::get_if<liquid::encoding::cbor::value::map>(&body->data);
    EXPECT_NE(fields, nullptr);
    for (auto& [key, value] : *fields) {
        if (key == liquid::encoding::cbor::value{"author"}) {
            value = liquid::encoding::cbor::value{claimed.taap_id};
        }
    }
    auto encoded = liquid::encoding::cbor::encode(*body);
    EXPECT_TRUE(encoded) << encoded.error().message;
    auto signed_value = liquid::runtime::block_on(signing_key.sign("drip", *encoded));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    drip.author = signing_key.identity();
    drip.author.taap_id = claimed.taap_id;
    drip.author.fingerprint = claimed.fingerprint;
    drip.body = std::move(*encoded);
    drip.signature = std::move(*signature);
    return drip;
}

// liquid#383: a peer sends a drip signed by its own key that names another identity as the
// author. The receiver does not hold that identity's chain, so the drip must be refused.
TEST(Sync, drip_signed_by_one_key_naming_another_identity_is_refused) {
    const auto restore = scoped_liquid_home{};
    temporary_directory victim_home;
    temporary_directory attacker_home;
    temporary_directory receiver_home;
    temporary_directory source_path;
    temporary_directory destination_path;

    scoped_liquid_home::select(victim_home.path());
    const auto victim = open_local_signer().identity();

    scoped_liquid_home::select(attacker_home.path());
    auto attacker = open_local_signer();
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    auto items = export_all(*source);
    ASSERT_TRUE(items) << items.error().message;
    const auto drip = std::ranges::find_if(*items, [](const liquid::sync_item& item) {
        return item.kind == liquid::sync_item_kind::drip;
    });
    ASSERT_NE(drip, items->end());
    const auto forged = std::vector{forge_drip(*drip, attacker, victim)};

    scoped_liquid_home::select(receiver_home.path());
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(destination) << destination.error().message;
    auto imported = liquid::runtime::block_on(destination->import_drips(forged));
    ASSERT_FALSE(imported) << "a drip signed by one key was imported as another identity's work";
    EXPECT_EQ(imported.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::is_unknown_signer(imported.error())) << imported.error().message;

    // Carrying the victim's real chain does not help: it does not contain the attacker's key.
    scoped_liquid_home::select(victim_home.path());
    auto victim_chain = liquid::load_identity_chain(victim.taap_id);
    ASSERT_TRUE(victim_chain) << victim_chain.error().message;
    auto chain_body = liquid::encode_identity_chain(*victim_chain);
    ASSERT_TRUE(chain_body) << chain_body.error().message;
    scoped_liquid_home::select(receiver_home.path());
    const auto with_chain = std::vector{liquid::sync_item{.kind = liquid::sync_item_kind::identity,
                                                          .author = victim,
                                                          .body = *chain_body},
                                        forged.front()};
    auto still_refused = liquid::runtime::block_on(destination->import_drips(with_chain));
    ASSERT_FALSE(still_refused);
    EXPECT_EQ(still_refused.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(still_refused.error().message, "signing key is not in the identity chain");
}

// liquid#429, first exploit: a member pushes a change whose author names a colleague's taap_id
// but carries, and is signed by, the member's own key. The change is refused on import, whether
// or not the receiver holds the colleague's chain.
TEST(Sync, change_signed_by_one_key_naming_another_identity_is_refused) {
    const auto restore = scoped_liquid_home{};
    temporary_directory victim_home;
    temporary_directory attacker_home;
    temporary_directory receiver_home;
    temporary_directory source_path;
    temporary_directory destination_path;

    scoped_liquid_home::select(victim_home.path());
    const auto victim = open_local_signer().identity();

    scoped_liquid_home::select(attacker_home.path());
    auto attacker = open_local_signer();
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));
    auto items = export_all(*source);
    ASSERT_TRUE(items) << items.error().message;
    const auto change = std::ranges::find_if(*items, [](const liquid::sync_item& item) {
        return item.kind == liquid::sync_item_kind::change;
    });
    ASSERT_NE(change, items->end());

    auto record = liquid::objects::decode_change(change->body);
    ASSERT_TRUE(record) << record.error().message;
    record->author.taap_id = victim.taap_id;
    record->author.fingerprint = victim.fingerprint;
    auto body = liquid::objects::encode_change_body(*record);
    ASSERT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(attacker.sign("change", *body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    ASSERT_TRUE(signature) << signature.error().message;
    record->sig = *signature;
    auto encoded = liquid::objects::encode(*record);
    ASSERT_TRUE(encoded) << encoded.error().message;
    change->author = attacker.identity();
    change->author.taap_id = victim.taap_id;
    change->author.fingerprint = victim.fingerprint;
    change->body = std::move(*encoded);
    change->signature = std::move(*signature);

    scoped_liquid_home::select(receiver_home.path());
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(destination) << destination.error().message;
    auto imported = liquid::runtime::block_on(destination->import_drips(*items));
    ASSERT_FALSE(imported) << "a change signed by one key was imported as another identity's work";
    EXPECT_EQ(imported.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::is_unknown_signer(imported.error())) << imported.error().message;

    // Carrying the colleague's real chain does not help: it does not contain the member's key.
    scoped_liquid_home::select(victim_home.path());
    auto victim_chain = liquid::load_identity_chain(victim.taap_id);
    ASSERT_TRUE(victim_chain) << victim_chain.error().message;
    auto chain_body = liquid::encode_identity_chain(*victim_chain);
    ASSERT_TRUE(chain_body) << chain_body.error().message;
    scoped_liquid_home::select(receiver_home.path());
    auto with_chain = std::vector{liquid::sync_item{
        .kind = liquid::sync_item_kind::identity, .author = victim, .body = *chain_body}};
    with_chain.insert(with_chain.end(), items->begin(), items->end());
    auto still_refused = liquid::runtime::block_on(destination->import_drips(with_chain));
    ASSERT_FALSE(still_refused);
    EXPECT_EQ(still_refused.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(still_refused.error().message, "signing key is not in the identity chain");
}

// liquid#429: history whose author's chain this machine does not hold (for example drips and
// changes signed before liquid#448 by an identity whose chain was never kept) is listed as
// signed by an unverified key, never as the author it names.
TEST(Sync, history_without_its_authors_chain_is_listed_as_unverified) {
    const auto restore = scoped_liquid_home{};
    temporary_directory author_home;
    temporary_directory reader_home;
    temporary_directory repository_path;

    scoped_liquid_home::select(author_home.path());
    {
        auto written =
            liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
        ASSERT_TRUE(written) << written.error().message;
        ASSERT_TRUE(liquid::runtime::block_on(written->write("one.txt", bytes("one"))));
        ASSERT_TRUE(liquid::runtime::block_on(written->cut("first")));
        auto drips = liquid::runtime::block_on(written->drips());
        ASSERT_TRUE(drips) << drips.error().message;
        ASSERT_FALSE(drips->empty());
        EXPECT_TRUE(std::ranges::all_of(*drips, &liquid::drip::identity_verified));
        auto changes = liquid::runtime::block_on(written->log());
        ASSERT_TRUE(changes) << changes.error().message;
        ASSERT_FALSE(changes->empty());
        EXPECT_TRUE(std::ranges::all_of(*changes, &liquid::change::identity_verified));
    }

    scoped_liquid_home::select(reader_home.path());
    auto opened = liquid::runtime::block_on(liquid::repository::open(repository_path.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    auto drips = liquid::runtime::block_on(opened->drips());
    ASSERT_TRUE(drips) << drips.error().message;
    ASSERT_FALSE(drips->empty());
    EXPECT_TRUE(std::ranges::none_of(*drips, &liquid::drip::identity_verified));
    auto changes = liquid::runtime::block_on(opened->log());
    ASSERT_TRUE(changes) << changes.error().message;
    ASSERT_FALSE(changes->empty());
    EXPECT_TRUE(std::ranges::none_of(*changes, &liquid::change::identity_verified));
}

// liquid#508: `blame`, `history`, `ops`, `why` and `wrap` print an author from path history,
// operation records, line attribution and change attribution. Opened from a home that holds no
// chain for the author, every one of them names someone no chain verified, exactly as a forged
// author would, and must be reported as unverified. In the author's own home they are verified.
TEST(Sync, attribution_surfaces_report_an_author_no_chain_verifies_as_unverified) {
    const auto restore = scoped_liquid_home{};
    temporary_directory author_home;
    temporary_directory reader_home;
    temporary_directory repository_path;

    const auto check = [](liquid::repository& opened, bool expected) {
        auto head = liquid::runtime::block_on(opened.head());
        ASSERT_TRUE(head) << head.error().message;

        auto history = liquid::runtime::block_on(opened.history("one.txt"));
        ASSERT_TRUE(history) << history.error().message;
        ASSERT_FALSE(history->empty());
        for (const auto& entry : *history) {
            EXPECT_EQ(entry.identity_verified, expected) << entry.path;
        }

        auto operations = liquid::runtime::block_on(opened.operations());
        ASSERT_TRUE(operations) << operations.error().message;
        ASSERT_FALSE(operations->empty());
        for (const auto& operation : *operations) {
            EXPECT_EQ(operation.identity_verified, expected) << operation.kind;
        }

        auto lines = liquid::runtime::block_on(opened.attribution("one.txt", *head));
        ASSERT_TRUE(lines) << lines.error().message;
        ASSERT_FALSE(lines->empty());
        for (const auto& line : *lines) {
            EXPECT_EQ(line.identity_verified, expected) << line.line;
        }

        auto entry = liquid::runtime::block_on(
            liquid::repository_detail::vfs_access::entry(opened, "one.txt"));
        ASSERT_TRUE(entry) << entry.error().message;
        auto changed = liquid::runtime::block_on(
            liquid::repository_detail::build_access::path_change(opened, "one.txt", entry->object));
        ASSERT_TRUE(changed) << changed.error().message;
        ASSERT_TRUE(*changed);
        EXPECT_EQ((*changed)->identity_verified, expected);
    };

    scoped_liquid_home::select(author_home.path());
    {
        auto written =
            liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
        ASSERT_TRUE(written) << written.error().message;
        ASSERT_TRUE(liquid::runtime::block_on(written->write("one.txt", bytes("one\ntwo\n"))));
        ASSERT_TRUE(liquid::runtime::block_on(written->cut("first")));
        check(*written, true);
    }

    scoped_liquid_home::select(reader_home.path());
    auto opened = liquid::runtime::block_on(liquid::repository::open(repository_path.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    check(*opened, false);
}

// liquid#508: the push rate limit is charged to the identity that authenticated the push, never to
// the author the pushed records claim. A pusher cannot spend, or borrow, another person's budget
// by naming them.
TEST(Sync, push_rate_limit_is_keyed_on_the_authenticated_pusher) {
    temporary_directory source_path;
    temporary_directory limited_path;
    temporary_directory allowed_path;
    const auto author = open_local_signer().identity();
    const auto pusher = std::string{"taap:authenticated-pusher"};
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    // The claimed author has a large budget and the pusher a budget of one: the push is refused
    // on the actor bucket, because it is charged to the pusher.
    auto limited_options = liquid::repository_options{};
    limited_options.flow.per_actor = {.rate_per_s = 1, .burst = 1};
    limited_options.flow.actor_overrides[author.taap_id] = {.rate_per_s = 100'000,
                                                            .burst = 100'000};
    auto limited =
        liquid::runtime::block_on(liquid::repository::create(limited_path.path(), limited_options));
    ASSERT_TRUE(limited) << limited.error().message;
    {
        auto server = liquid::sync_server{*limited, {.pusher = pusher}};
        sync_http_server http{server};
        auto endpoint = liquid::sync_endpoint{http.url()};
        auto pushed = liquid::runtime::block_on(endpoint.push(*source));
        ASSERT_TRUE(pushed) << (pushed ? "" : pushed.error().message);
        EXPECT_GT(pushed->retry_after_ms, 0U);
        EXPECT_EQ(pushed->exhausted_scope, "actor");
        EXPECT_FALSE(liquid::runtime::block_on(limited->head()));
    }

    // The reverse: the claimed author's budget is one and the pusher's is large. The push is
    // admitted, because the author's bucket is never charged.
    auto allowed_options = liquid::repository_options{};
    allowed_options.flow.per_actor = {.rate_per_s = 1, .burst = 1};
    allowed_options.flow.actor_overrides[pusher] = {.rate_per_s = 100'000, .burst = 100'000};
    auto allowed =
        liquid::runtime::block_on(liquid::repository::create(allowed_path.path(), allowed_options));
    ASSERT_TRUE(allowed) << allowed.error().message;
    {
        auto server = liquid::sync_server{*allowed, {.pusher = pusher}};
        sync_http_server http{server};
        auto endpoint = liquid::sync_endpoint{http.url()};
        auto pushed = liquid::runtime::block_on(endpoint.push(*source));
        ASSERT_TRUE(pushed) << (pushed ? "" : pushed.error().message);
        EXPECT_EQ(pushed->retry_after_ms, 0U);
        EXPECT_TRUE(pushed->exhausted_scope.empty());
        EXPECT_TRUE(liquid::runtime::block_on(allowed->head()));
    }
}

// The honest case: two people on two machines. The sender's chain travels ahead of its drips and
// change, and the receiver verifies and stores it before checking their signatures.
TEST(Sync, author_identity_chain_travels_with_its_drips) {
    const auto restore = scoped_liquid_home{};
    temporary_directory author_home;
    temporary_directory receiver_home;
    temporary_directory source_path;
    temporary_directory destination_path;

    scoped_liquid_home::select(author_home.path());
    const auto author = open_local_signer().identity();
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("one.txt", bytes("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));
    auto items = export_all(*source);
    ASSERT_TRUE(items) << items.error().message;
    ASSERT_FALSE(items->empty());
    EXPECT_EQ(items->front().kind, liquid::sync_item_kind::identity);
    EXPECT_EQ(items->front().author.taap_id, author.taap_id);

    scoped_liquid_home::select(receiver_home.path());
    EXPECT_FALSE(liquid::load_identity_chain(author.taap_id));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(destination) << destination.error().message;
    auto without_chain = std::vector<liquid::sync_item>{};
    std::ranges::copy_if(*items, std::back_inserter(without_chain), [](const auto& item) {
        return item.kind != liquid::sync_item_kind::identity;
    });
    auto refused = liquid::runtime::block_on(destination->import_drips(without_chain));
    ASSERT_FALSE(refused);
    EXPECT_TRUE(liquid::is_unknown_signer(refused.error())) << refused.error().message;

    auto imported = liquid::runtime::block_on(destination->import_drips(*items));
    ASSERT_TRUE(imported) << imported.error().message;
    auto stored = liquid::load_identity_chain(author.taap_id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(stored->front().subject.taap_id, author.taap_id);
    auto head = liquid::runtime::block_on(destination->head());
    ASSERT_TRUE(head) << head.error().message;

    // The receiver relays the author's chain to the next machine, even from a new process home
    // (a restarted container): the repository keeps every author's chain with its records.
    temporary_directory restarted_home;
    scoped_liquid_home::select(restarted_home.path());
    EXPECT_FALSE(liquid::load_identity_chain(author.taap_id));
    auto relayed = export_all(*destination);
    ASSERT_TRUE(relayed) << relayed.error().message;
    EXPECT_TRUE(std::ranges::any_of(*relayed, [&](const liquid::sync_item& item) {
        return item.kind == liquid::sync_item_kind::identity &&
               item.author.taap_id == author.taap_id;
    }));
}

auto write_drive_file(const std::filesystem::path& path, std::string_view value) -> void {
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    ASSERT_TRUE(output);
}

auto read_drive_file(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

auto has_drive_outcome(const liquid::drive_sync_report& report,
                       liquid::drive_reconciliation_kind kind,
                       std::string_view path = {}) -> bool {
    return std::ranges::any_of(report.paths, [&](const liquid::drive_reconciliation& entry) {
        return entry.kind == kind && (path.empty() || entry.path == path);
    });
}

auto peak_rss_bytes() -> std::uint64_t {
    auto usage = rusage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}

TEST(Drive, seeded_reconciliation_covers_every_case_and_converges_through_sync_server) {
    constexpr auto seed = std::uint64_t{195};
    auto random = std::mt19937_64{seed};
    temporary_directory folder;
    temporary_directory server_path;
    temporary_directory ordinary_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    auto server = liquid::sync_server{*server_repository};
    sync_http_server http{server};

    const auto local_value = "local-" + std::to_string(random());
    write_drive_file(folder.path() / "local.txt", local_value);
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client) << client.error().message << ": " << client.error().context;
    auto first = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    EXPECT_TRUE(
        has_drive_outcome(*first, liquid::drive_reconciliation_kind::local_only, "local.txt"));
    ASSERT_TRUE(first->status.trees.base);
    ASSERT_TRUE(first->status.trees.local);
    ASSERT_TRUE(first->status.trees.remote);
    EXPECT_EQ(liquid::to_string(*first->status.trees.base),
              liquid::to_string(*first->status.trees.local));

    auto ordinary = liquid::runtime::block_on(liquid::repository::create(ordinary_path.path()));
    ASSERT_TRUE(ordinary);
    auto endpoint = liquid::sync_endpoint{http.url()};
    auto ordinary_pull = liquid::runtime::block_on(endpoint.pull(*ordinary));
    ASSERT_TRUE(ordinary_pull) << ordinary_pull.error().message;
    auto ordinary_local = liquid::runtime::block_on(ordinary->read("local.txt"));
    ASSERT_TRUE(ordinary_local);
    EXPECT_EQ(text(*ordinary_local), local_value);

    const auto remote_value = "remote-" + std::to_string(random());
    ASSERT_TRUE(liquid::runtime::block_on(
        server_repository->write("nested/remote.txt", bytes(remote_value))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("remote only")));
    auto remote_only = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(remote_only) << remote_only.error().message << ": " << remote_only.error().context;
    EXPECT_TRUE(has_drive_outcome(
        *remote_only, liquid::drive_reconciliation_kind::remote_only, "nested/remote.txt"));
    EXPECT_EQ(read_drive_file(folder.path() / "nested/remote.txt"), remote_value);

    ASSERT_TRUE(liquid::runtime::block_on(server_repository->write("shared.txt", bytes("base"))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("conflict base")));
    auto initial_sync = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(initial_sync) << initial_sync.error().message << ": "
                              << initial_sync.error().context;
    const auto left = "left-" + std::to_string(random());
    const auto right = "right-" + std::to_string(random());
    write_drive_file(folder.path() / "shared.txt", left);
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->write("shared.txt", bytes(right))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("remote concurrent edit")));
    auto conflicted = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(conflicted) << conflicted.error().message << ": " << conflicted.error().context;
    EXPECT_TRUE(has_drive_outcome(
        *conflicted, liquid::drive_reconciliation_kind::content_conflict, "shared.txt"));
    EXPECT_EQ(read_drive_file(folder.path() / "shared.txt"), left);
    EXPECT_EQ(read_drive_file(folder.path() / "shared (remote conflict).txt"), right);
    ASSERT_EQ(conflicted->status.conflicts.size(), 1U);
    EXPECT_EQ(conflicted->status.conflicts.front().region.path, "shared.txt");
    EXPECT_EQ(conflicted->status.conflicts.front().kind, liquid::conflict_kind::structural);

    std::filesystem::rename(folder.path() / "nested/remote.txt", folder.path() / "moved.txt");
    auto moved = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(moved) << moved.error().message << ": " << moved.error().context;
    EXPECT_TRUE(has_drive_outcome(*moved, liquid::drive_reconciliation_kind::moved, "moved.txt"));
    EXPECT_FALSE(std::filesystem::exists(folder.path() / "nested/remote.txt"));
    EXPECT_EQ(read_drive_file(folder.path() / "moved.txt"), remote_value);
    ASSERT_EQ(moved->status.conflicts.size(), 1U);
    EXPECT_EQ(moved->status.conflicts.front().region.path, "shared.txt");

    write_drive_file(folder.path() / "shared.txt", "resolved");
    std::filesystem::remove(folder.path() / "shared (remote conflict).txt");
    auto resolved = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(resolved) << resolved.error().message << ": " << resolved.error().context;
    EXPECT_TRUE(resolved->status.conflicts.empty());

    write_drive_file(folder.path() / "delete-edit.txt", "base");
    ASSERT_TRUE(liquid::runtime::block_on(client->sync()));
    std::filesystem::remove(folder.path() / "delete-edit.txt");
    ASSERT_TRUE(liquid::runtime::block_on(
        server_repository->write("delete-edit.txt", bytes("edited remotely"))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("edit against delete")));
    auto delete_edit = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(delete_edit) << delete_edit.error().message << ": " << delete_edit.error().context;
    EXPECT_TRUE(has_drive_outcome(
        *delete_edit, liquid::drive_reconciliation_kind::delete_edit, "delete-edit.txt"));
    EXPECT_EQ(read_drive_file(folder.path() / "delete-edit.txt"), "edited remotely");

    auto large = std::string(256U * 1024U, 'x');
    large.front() = static_cast<char>('a' + random() % 26U);
    ASSERT_TRUE(
        liquid::runtime::block_on(server_repository->write("interrupted.bin", bytes(large))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("interrupted transfer")));
    http.fail_next_chunk_read();
    auto interrupted = liquid::runtime::block_on(client->sync());
    ASSERT_FALSE(interrupted);
    auto interrupted_status = liquid::runtime::block_on(client->status());
    ASSERT_TRUE(interrupted_status);
    EXPECT_TRUE(interrupted_status->trees.interrupted);
    auto resumed = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(resumed) << resumed.error().message << ": " << resumed.error().context;
    EXPECT_TRUE(
        has_drive_outcome(*resumed, liquid::drive_reconciliation_kind::interrupted_transfer));
    EXPECT_EQ(read_drive_file(folder.path() / "interrupted.bin"), large);
    EXPECT_FALSE(resumed->status.trees.interrupted);

    const auto walks_before = resumed->status.full_walks;
    write_drive_file(folder.path() / "lost-event.txt", "found by full walk");
    auto overflow_recovery = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(overflow_recovery) << overflow_recovery.error().message;
    EXPECT_GT(overflow_recovery->status.full_walks, walks_before);
    auto server_lost_event = liquid::runtime::block_on(server_repository->read("lost-event.txt"));
    ASSERT_TRUE(server_lost_event);
    EXPECT_EQ(text(*server_lost_event), "found by full walk");
    EXPECT_LT(overflow_recovery->status.state_bytes, 256ULL * 1024ULL * 1024ULL);
}

TEST(Drive, concurrent_offline_edits_from_two_clients_keep_both_named_versions) {
    temporary_directory first_folder;
    temporary_directory second_folder;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    auto server = liquid::sync_server{*server_repository};
    sync_http_server http{server};

    write_drive_file(first_folder.path() / "shared.txt", "initial");
    auto first = liquid::runtime::block_on(
        liquid::drive_client::open(first_folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    auto initial_upload = liquid::runtime::block_on(first->sync());
    ASSERT_TRUE(initial_upload) << initial_upload.error().message << ": "
                                << initial_upload.error().context;

    auto second = liquid::runtime::block_on(
        liquid::drive_client::open(second_folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    auto initial_download = liquid::runtime::block_on(second->sync());
    ASSERT_TRUE(initial_download) << initial_download.error().message << ": "
                                  << initial_download.error().context;
    ASSERT_EQ(read_drive_file(second_folder.path() / "shared.txt"), "initial");

    write_drive_file(first_folder.path() / "shared.txt", "first offline edit");
    write_drive_file(second_folder.path() / "shared.txt", "second offline edit");
    auto first_upload = liquid::runtime::block_on(first->sync());
    ASSERT_TRUE(first_upload) << first_upload.error().message << ": "
                              << first_upload.error().context;
    auto second_upload = liquid::runtime::block_on(second->sync());
    ASSERT_TRUE(second_upload) << second_upload.error().message << ": "
                               << second_upload.error().context;

    EXPECT_EQ(read_drive_file(second_folder.path() / "shared.txt"), "second offline edit");
    EXPECT_EQ(read_drive_file(second_folder.path() / "shared (remote conflict).txt"),
              "first offline edit");
    ASSERT_EQ(second_upload->status.conflicts.size(), 1U);
    EXPECT_EQ(second_upload->status.conflicts.front().region.path, "shared.txt");
    EXPECT_EQ(second_upload->status.conflicts.front().kind, liquid::conflict_kind::structural);
    EXPECT_NE(liquid::to_string(second_upload->status.conflicts.front().base),
              liquid::to_string(liquid::digest{}));

    auto recorded = liquid::runtime::block_on(server_repository->conflict_at("shared.txt"));
    ASSERT_TRUE(recorded) << recorded.error().message;
    EXPECT_EQ(recorded->kind, liquid::conflict_kind::structural);
    EXPECT_EQ(recorded->region.path, "shared.txt");

    auto original = liquid::runtime::block_on(server_repository->read("shared.txt"));
    ASSERT_TRUE(original) << original.error().message;
    EXPECT_EQ(text(*original), "second offline edit");
    auto conflict =
        liquid::runtime::block_on(server_repository->read("shared (remote conflict).txt"));
    ASSERT_TRUE(conflict) << conflict.error().message;
    EXPECT_EQ(text(*conflict), "first offline edit");
    auto original_history = liquid::runtime::block_on(server_repository->history("shared.txt"));
    ASSERT_TRUE(original_history) << original_history.error().message;
    auto conflict_history =
        liquid::runtime::block_on(server_repository->history("shared (remote conflict).txt"));
    ASSERT_TRUE(conflict_history) << conflict_history.error().message;
    EXPECT_FALSE(original_history->empty());
    EXPECT_FALSE(conflict_history->empty());
}

TEST(Drive, state_memory_ceiling_refuses_an_unbounded_walk) {
    constexpr auto file_count = 4096;
    constexpr auto state_ceiling = std::uint64_t{256} * 1024U;
    constexpr auto rss_ceiling = std::uint64_t{1} * 1024U * 1024U * 1024U;
    temporary_directory folder;
    for (auto index = 0; index < file_count; ++index) {
        write_drive_file(folder.path() / ("file-" + std::to_string(index)), "value");
    }
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(),
                                   "http://127.0.0.1:1",
                                   {.state_memory_ceiling = state_ceiling, .durable = false}));
    ASSERT_TRUE(client);
    auto synchronized = liquid::runtime::block_on(client->sync());
    ASSERT_FALSE(synchronized);
    EXPECT_EQ(synchronized.error().code, liquid::errc::busy);
    EXPECT_EQ(synchronized.error().message, "drive state memory ceiling exceeded");
    EXPECT_EQ(synchronized.error().context, std::to_string(state_ceiling));
    ASSERT_GT(peak_rss_bytes(), 0U);
    EXPECT_LT(peak_rss_bytes(), rss_ceiling);
}

TEST(Drive, path_states_settings_and_hydration_records_survive_reopen) {
    temporary_directory folder;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    ASSERT_TRUE(liquid::runtime::block_on(
        server_repository->write("photos/2026/image.jpg", bytes("image"))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("add image")));
    auto server = liquid::sync_server{*server_repository};
    sync_http_server http{server};
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client);
    ASSERT_TRUE(liquid::runtime::block_on(
        client->configure({.hydration_size_threshold = 4, .pin_folders = true})));
    auto initial_sync = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(initial_sync) << initial_sync.error().message << ": "
                              << initial_sync.error().context;
    EXPECT_FALSE(std::filesystem::exists(folder.path() / "photos/2026/image.jpg"));
    EXPECT_EQ(initial_sync->status.files, 1U);
    EXPECT_EQ(initial_sync->status.hydration_debt, 5U);
    auto repeated_sync = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(repeated_sync) << repeated_sync.error().message << ": "
                               << repeated_sync.error().context;
    EXPECT_FALSE(std::filesystem::exists(folder.path() / "photos/2026/image.jpg"));
    EXPECT_EQ(repeated_sync->status.hydration_debt, 5U);
    http.fail_next_chunk_read();
    auto unavailable = liquid::runtime::block_on(client->pin("photos"));
    ASSERT_FALSE(unavailable);
    auto offline = liquid::runtime::block_on(client->status());
    ASSERT_TRUE(offline);
    EXPECT_NE(offline->offline_reason.find("pinned content is unavailable"), std::string::npos);
    EXPECT_EQ(offline->hydration_debt, 5U);
    ASSERT_TRUE(liquid::runtime::block_on(client->pin("photos")));
    auto pinned = liquid::runtime::block_on(client->path_status("photos/2026/image.jpg"));
    ASSERT_TRUE(pinned);
    EXPECT_EQ(pinned->state, liquid::drive_path_state::available_offline);
    EXPECT_TRUE(pinned->hydrated);
    EXPECT_EQ(pinned->hydration_debt, 0U);
    ASSERT_TRUE(liquid::runtime::block_on(client->unpin("photos/2026/image.jpg")));
    auto unpinned = liquid::runtime::block_on(client->path_status("photos/2026/image.jpg"));
    ASSERT_TRUE(unpinned);
    EXPECT_EQ(unpinned->state, liquid::drive_path_state::online_only);
    ASSERT_TRUE(liquid::runtime::block_on(
        client->set_state("photos/private", liquid::drive_path_state::not_synced_here)));

    auto reopened = liquid::runtime::block_on(liquid::drive_client::open(folder.path()));
    ASSERT_TRUE(reopened);
    auto settings = liquid::runtime::block_on(reopened->settings());
    ASSERT_TRUE(settings);
    EXPECT_EQ(settings->hydration_size_threshold, 4U);
    EXPECT_TRUE(settings->pin_folders);
    auto persisted = liquid::runtime::block_on(reopened->path_status("photos/2026/image.jpg"));
    ASSERT_TRUE(persisted);
    EXPECT_EQ(persisted->state, liquid::drive_path_state::online_only);
    auto status = liquid::runtime::block_on(reopened->status());
    ASSERT_TRUE(status);
    EXPECT_EQ(status->hydration_debt, 0U);
    EXPECT_EQ(status->pending_uploads, 0U);
    const auto excluded =
        std::ranges::find(status->paths, "photos/private", &liquid::drive_path_status::path);
    ASSERT_NE(excluded, status->paths.end());
    EXPECT_EQ(excluded->state, liquid::drive_path_state::not_synced_here);
}

TEST(Drive, repository_writes_queue_offline_and_reconcile_later) {
    temporary_directory folder;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    auto server = liquid::sync_server{*server_repository};
    sync_http_server http{server};
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client);
    auto initial_sync = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(initial_sync) << initial_sync.error().message << ": "
                              << initial_sync.error().context;
    client = liquid::drive_client{};

    {
        auto repository = liquid::runtime::block_on(liquid::repository::open(folder.path()));
        ASSERT_TRUE(repository);
        ASSERT_TRUE(liquid::runtime::block_on(repository->write("queued.txt", bytes("queued"))));
        ASSERT_TRUE(liquid::runtime::block_on(repository->cut("queue drive write")));
    }

    auto reopened = liquid::runtime::block_on(liquid::drive_client::open(folder.path()));
    ASSERT_TRUE(reopened);
    auto pending = liquid::runtime::block_on(reopened->status());
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->pending_uploads, 1U);
    auto synchronized = liquid::runtime::block_on(reopened->sync());
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    EXPECT_EQ(synchronized->status.pending_uploads, 0U);
    auto uploaded = liquid::runtime::block_on(server_repository->read("queued.txt"));
    ASSERT_TRUE(uploaded);
    EXPECT_EQ(text(*uploaded), "queued");
}

TEST(Drive, refused_reference_preserves_reason_and_unadvanced_base) {
    temporary_directory folder;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    auto authorize = [](const liquid::sync_item& item,
                        std::span<const liquid::sync_ref_update> updates)
        -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        if (item.kind == liquid::sync_item_kind::change) {
            co_return refused_verdicts(updates, "main", "drive write policy refused the change");
        }
        co_return accepted_verdicts(updates);
    };
    auto server = liquid::sync_server{*server_repository,
                                      liquid::sync_server_options{.authorize = authorize}};
    sync_http_server http{server};
    write_drive_file(folder.path() / "local.txt", "local");
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client);
    auto synchronized = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    ASSERT_EQ(synchronized->status.references.size(), 1U);
    EXPECT_EQ(synchronized->status.references.front().name, "main");
    EXPECT_EQ(synchronized->status.references.front().outcome,
              liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(synchronized->status.references.front().reason,
              "drive write policy refused the change");
    EXPECT_FALSE(synchronized->status.trees.base);
    EXPECT_FALSE(synchronized->status.trees.interrupted);

    auto reopened = liquid::runtime::block_on(liquid::drive_client::open(folder.path()));
    ASSERT_TRUE(reopened);
    auto status = liquid::runtime::block_on(reopened->status());
    ASSERT_TRUE(status);
    ASSERT_EQ(status->references.size(), 1U);
    EXPECT_EQ(status->references.front().reason, "drive write policy refused the change");
    EXPECT_FALSE(status->trees.base);
}

TEST(Drive, symlink_targets_never_enter_the_synced_repository) {
    temporary_directory folder;
    temporary_directory outside;
    temporary_directory server_path;
    const auto secret = std::string{"dummy host credential LSR-029"};
    write_drive_file(outside.path() / "secret.txt", secret);
    write_drive_file(folder.path() / "regular.txt", "ordinary content");
    std::filesystem::create_symlink(outside.path() / "secret.txt", folder.path() / "absolute.txt");
    std::filesystem::create_symlink(
        (outside.path() / "secret.txt").lexically_relative(folder.path()),
        folder.path() / "relative.txt");
    std::filesystem::create_symlink("regular.txt", folder.path() / "internal.txt");
    std::filesystem::create_symlink("missing.txt", folder.path() / "dangling.txt");
    std::filesystem::create_directory_symlink(outside.path(), folder.path() / "directory");
    auto remote = liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(remote);
    auto server = liquid::sync_server{*remote};
    sync_http_server http{server};
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client);
    auto first = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(first) << first.error().message;
    for (const auto* path :
         {"absolute.txt", "relative.txt", "internal.txt", "dangling.txt", "directory/secret.txt"}) {
        EXPECT_FALSE(liquid::runtime::block_on(remote->read(path))) << path;
    }
    auto ordinary = liquid::runtime::block_on(remote->read("regular.txt"));
    ASSERT_TRUE(ordinary);
    EXPECT_EQ(text(*ordinary), "ordinary content");

    // A later full walk must apply the same rule to an already synchronized name.
    std::filesystem::remove(folder.path() / "regular.txt");
    std::filesystem::create_symlink(outside.path() / "secret.txt", folder.path() / "regular.txt");
    auto later = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(later) << later.error().message;
    auto retained = liquid::runtime::block_on(remote->read("regular.txt"));
    ASSERT_TRUE(retained);
    EXPECT_EQ(text(*retained), "ordinary content");
    auto secret_id = liquid::hash(liquid::algorithm::blake3, bytes(secret));
    ASSERT_TRUE(secret_id);
    auto present = liquid::runtime::block_on(
        liquid::repository_detail::drive_access::objects(*remote).contains(*secret_id));
    ASSERT_TRUE(present);
    EXPECT_FALSE(*present);
}

TEST(Drive, staging_refuses_links_and_invalid_sources_even_without_the_walk) {
    temporary_directory folder;
    temporary_directory outside;
    temporary_directory repository_path;
    write_drive_file(folder.path() / "nested/file.txt", "safe");
    write_drive_file(outside.path() / "file.txt", "dummy host credential");
    auto repository = liquid::runtime::block_on(liquid::repository::create(repository_path.path()));
    ASSERT_TRUE(repository);
    auto stage = [&](const std::filesystem::path& relative) {
        return liquid::runtime::block_on(liquid::repository_detail::drive_access::stage(
            *repository, folder.path(), relative, "file.txt", 0100644));
    };
    ASSERT_TRUE(stage("nested/file.txt"));
    std::filesystem::remove(folder.path() / "nested/file.txt");
    std::filesystem::create_symlink(outside.path() / "file.txt", folder.path() / "nested/file.txt");
    EXPECT_FALSE(stage("nested/file.txt"));
    std::filesystem::remove_all(folder.path() / "nested");
    std::filesystem::create_directory_symlink(outside.path(), folder.path() / "nested");
    EXPECT_FALSE(stage("nested/file.txt"));
    EXPECT_FALSE(stage(outside.path() / "file.txt"));
    EXPECT_FALSE(stage("../file.txt"));
    EXPECT_FALSE(stage("nested/../file.txt"));
    EXPECT_FALSE(stage(std::string{"file.txt\0ignored", 16}));
    EXPECT_FALSE(stage(""));
    EXPECT_FALSE(stage("nested"));
    ASSERT_EQ(::mkfifo((folder.path() / "pipe").c_str(), 0600), 0);
    EXPECT_FALSE(stage("pipe"));
}

TEST(Drive, pinned_source_survives_file_and_parent_replacement) {
    temporary_directory folder;
    temporary_directory outside;
    write_drive_file(folder.path() / "nested/file.txt", "original safe bytes");
    write_drive_file(outside.path() / "file.txt", "dummy host credential");
    auto source = liquid::drive_detail::source_file::open(folder.path(), "nested/file.txt");
    ASSERT_TRUE(source);
    auto read_source = [&] {
        auto input = std::ifstream{source->path(), std::ios::binary};
        input.seekg(0);
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    std::filesystem::remove(folder.path() / "nested/file.txt");
    std::filesystem::create_symlink(outside.path() / "file.txt", folder.path() / "nested/file.txt");
    EXPECT_EQ(read_source(), "original safe bytes");
    std::filesystem::rename(folder.path() / "nested", folder.path() / "renamed");
    std::filesystem::create_directory_symlink(outside.path(), folder.path() / "nested");
    EXPECT_EQ(read_source(), "original safe bytes");

    class observed_write_through final : public liquid::blob_write_through {
    public:
        std::string received;
        liquid::runtime::task<void> put(liquid::digest,
                                        std::span<const std::byte> contents,
                                        std::span<const liquid::digest>,
                                        std::span<const std::uint64_t>) override {
            received = text(contents);
            co_return liquid::result<void>{};
        }
    };
    auto hook = std::make_shared<observed_write_through>();
    temporary_directory store_path;
    auto store =
        liquid::runtime::block_on(liquid::store::open(store_path.path(), {.write_through = hook}));
    ASSERT_TRUE(store);
    auto content = liquid::runtime::block_on(store->put_file(source->path()));
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(hook->received, "original safe bytes");
}

TEST(Drive, fresh_folder_joins_an_existing_remote_repository) {
    temporary_directory folder;
    temporary_directory server_path;
    auto server_repository =
        liquid::runtime::block_on(liquid::repository::create(server_path.path()));
    ASSERT_TRUE(server_repository);
    ASSERT_TRUE(
        liquid::runtime::block_on(server_repository->write("remote.txt", bytes("already remote"))));
    ASSERT_TRUE(liquid::runtime::block_on(server_repository->cut("existing remote")));
    auto server = liquid::sync_server{*server_repository};
    sync_http_server http{server};
    write_drive_file(folder.path() / "local.txt", "new local");
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder.path(), http.url(), {.durable = false}));
    ASSERT_TRUE(client);
    auto synchronized = liquid::runtime::block_on(client->sync());
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    EXPECT_EQ(read_drive_file(folder.path() / "remote.txt"), "already remote");
    auto server_local = liquid::runtime::block_on(server_repository->read("local.txt"));
    ASSERT_TRUE(server_local);
    EXPECT_EQ(text(*server_local), "new local");
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
    temporary_directory drive_path;
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
    const auto drive_output = source_path.path() / "drive-output.txt";
    const auto drive_status_output = source_path.path() / "drive-status-output.txt";
    const auto drive_pin_output = source_path.path() / "drive-pin-output.txt";
    const auto push = "cd " + shell_quote(source_path.path()) + " && " + executable + " push '" +
                      url + "' > " + shell_quote(push_output);
    const auto atomic_push = "cd " + shell_quote(source_path.path()) + " && " + executable +
                             " push --atomic '" + url + "' > " + shell_quote(atomic_output);
    const auto pull =
        "cd " + shell_quote(destination_path.path()) + " && " + executable + " pull '" + url + "'";
    const auto drive_sync = executable + " drive sync " + shell_quote(drive_path.path()) + " '" +
                            url + "' > " + shell_quote(drive_output);
    const auto drive_status = executable + " drive status " + shell_quote(drive_path.path()) +
                              " > " + shell_quote(drive_status_output);
    const auto drive_pin = executable + " drive pin " + shell_quote(drive_path.path() / "cli.txt") +
                           " > " + shell_quote(drive_pin_output);
    const auto drive_unpin = executable + " drive unpin " +
                             shell_quote(drive_path.path() / "cli.txt") + " > " +
                             shell_quote(drive_pin_output);
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
    write_drive_file(drive_path.path() / "drive.txt", "from drive");
    ASSERT_EQ(std::system(drive_sync.c_str()), 0);
    EXPECT_EQ(read_drive_file(drive_path.path() / "cli.txt"), "round trip");
    auto drive_report = std::ifstream{drive_output};
    const auto drive_report_text = std::string{std::istreambuf_iterator<char>{drive_report}, {}};
    EXPECT_NE(drive_report_text.find("reference=main outcome=applied"), std::string::npos);
    EXPECT_NE(drive_report_text.find("path=drive.txt outcome=local-only"), std::string::npos);
    ASSERT_EQ(std::system(drive_status.c_str()), 0);
    auto drive_status_report = std::ifstream{drive_status_output};
    const auto drive_status_text =
        std::string{std::istreambuf_iterator<char>{drive_status_report}, {}};
    EXPECT_NE(drive_status_text.find("remote=" + url), std::string::npos);
    EXPECT_NE(drive_status_text.find("interrupted=false"), std::string::npos);
    ASSERT_EQ(std::system(drive_pin.c_str()), 0);
    auto drive_pin_report = std::ifstream{drive_pin_output};
    auto drive_pin_text = std::string{std::istreambuf_iterator<char>{drive_pin_report}, {}};
    EXPECT_NE(drive_pin_text.find("state=available-offline"), std::string::npos);
    drive_pin_report.close();
    ASSERT_EQ(std::system(drive_unpin.c_str()), 0);
    drive_pin_report = std::ifstream{drive_pin_output};
    drive_pin_text = std::string{std::istreambuf_iterator<char>{drive_pin_report}, {}};
    EXPECT_NE(drive_pin_text.find("state=online-only"), std::string::npos);
    ASSERT_EQ(std::system(pull.c_str()), 0);
    auto opened = liquid::runtime::block_on(liquid::repository::open(destination_path.path()));
    ASSERT_TRUE(opened);
    auto content = liquid::runtime::block_on(opened->read("cli.txt"));
    ASSERT_TRUE(content) << content.error().message << " " << content.error().context;
    EXPECT_EQ(text(*content), "round trip");
    auto drive_content = liquid::runtime::block_on(opened->read("drive.txt"));
    ASSERT_TRUE(drive_content);
    EXPECT_EQ(text(*drive_content), "from drive");
}

TEST(Sync, cli_clone_materializes_and_builds_http_repository) {
    temporary_directory source_path;
    temporary_directory clone_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source);
    const auto project = std::string_view{"cmake_minimum_required(VERSION 3.28)\n"
                                          "project(clone_proof LANGUAGES CXX)\n"
                                          "add_executable(clone-proof main.cpp)\n"};
    ASSERT_TRUE(liquid::runtime::block_on(source->write("CMakeLists.txt", bytes(project))));
    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("main.cpp", bytes("int main() { return 0; }\n"))));
    auto large = std::vector<std::byte>(9U * 1024U * 1024U, std::byte{'x'});
    ASSERT_TRUE(liquid::runtime::block_on(source->write("large.bin", std::move(large))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("clone fixture")));
    auto server = liquid::sync_server{*source};
    auto http = sync_http_server{server};
    const auto output = source_path.path() / "clone.json";
    const auto executable = shell_quote(LIQUID_CLI);
    const auto clone = executable + " clone '" + http.url() + "' " +
                       shell_quote(clone_path.path()) + " --json > " + shell_quote(output);
    ASSERT_EQ(std::system(clone.c_str()), 0);

    EXPECT_TRUE(std::filesystem::exists(clone_path.path() / "LIQUID_REPO"));
    EXPECT_TRUE(std::filesystem::exists(clone_path.path() / "CMakeLists.txt"));
    EXPECT_EQ(std::filesystem::file_size(clone_path.path() / "large.bin"), 9U * 1024U * 1024U);
    auto report = std::ifstream{output};
    const auto report_text = std::string{std::istreambuf_iterator<char>{report}, {}};
    EXPECT_NE(report_text.find("\"path\":"), std::string::npos);
    EXPECT_NE(report_text.find("\"remote\":\"" + http.url() + "\""), std::string::npos);

    auto drive = liquid::runtime::block_on(liquid::drive_client::open(clone_path.path()));
    ASSERT_TRUE(drive);
    auto status = liquid::runtime::block_on(drive->status());
    ASSERT_TRUE(status);
    EXPECT_EQ(status->remote, http.url());

    const auto build_path = clone_path.path() / "build";
    const auto configure = "cmake -S " + shell_quote(clone_path.path()) + " -B " +
                           shell_quote(build_path) + " -G Ninja";
    const auto build = "cmake --build " + shell_quote(build_path) + " -j 2";
    ASSERT_EQ(std::system(configure.c_str()), 0);
    ASSERT_EQ(std::system(build.c_str()), 0);
    EXPECT_TRUE(std::filesystem::exists(build_path / "clone-proof"));

    const auto second_clone_path = source_path.path() / "drive-folder-clone";
    const auto clone_from_drive = executable + " clone " + shell_quote(clone_path.path()) + " " +
                                  shell_quote(second_clone_path);
    ASSERT_EQ(std::system(clone_from_drive.c_str()), 0);
    EXPECT_TRUE(std::filesystem::exists(second_clone_path / "CMakeLists.txt"));
    auto second_drive = liquid::runtime::block_on(liquid::drive_client::open(second_clone_path));
    ASSERT_TRUE(second_drive);
    auto second_status = liquid::runtime::block_on(second_drive->status());
    ASSERT_TRUE(second_status);
    EXPECT_EQ(second_status->remote, http.url());
}

TEST(Sync, cli_clone_names_target_authentication_and_repository_refusals) {
    temporary_directory source_path;
    temporary_directory target_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("file.txt", bytes("content"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("clone fixture")));
    auto server = liquid::sync_server{*source};
    const auto executable = shell_quote(LIQUID_CLI);

    std::filesystem::create_directories(target_path.path());
    std::ofstream{target_path.path() / "keep.txt"} << "keep";
    auto http = sync_http_server{server};
    const auto nonempty_error = source_path.path() / "nonempty-error.txt";
    const auto nonempty = executable + " clone '" + http.url() + "' " +
                          shell_quote(target_path.path()) + " 2> " + shell_quote(nonempty_error);
    EXPECT_NE(std::system(nonempty.c_str()), 0);
    auto error_input = std::ifstream{nonempty_error};
    auto error_text = std::string{std::istreambuf_iterator<char>{error_input}, {}};
    EXPECT_NE(error_text.find("already_exists: clone target is not empty"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(target_path.path() / "keep.txt"));

    auto authenticated = sync_http_server{server, false, "clone-token"};
    const auto auth_work = source_path.path() / "auth-work";
    const auto auth_target = auth_work / "repository";
    const auto auth_error = source_path.path() / "auth-error.txt";
    std::filesystem::create_directories(auth_work);
    const auto auth = "cd " + shell_quote(auth_work) + " && " + executable + " clone '" +
                      authenticated.url() + "' 2> " + shell_quote(auth_error);
    EXPECT_NE(std::system(auth.c_str()), 0);
    error_input = std::ifstream{auth_error};
    error_text = std::string{std::istreambuf_iterator<char>{error_input}, {}};
    EXPECT_NE(error_text.find("unauthorized: repository authentication required"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(auth_target));
    const auto authorized = "cd " + shell_quote(auth_work) + " && LIQUID_TOKEN=clone-token " +
                            executable + " clone '" + authenticated.url() + "' --json";
    EXPECT_EQ(std::system(authorized.c_str()), 0);
    EXPECT_TRUE(std::filesystem::exists(auth_target / "file.txt"));

    auto missing = sync_http_server{server, false, {}, false};
    const auto missing_target = source_path.path() / "missing-target";
    const auto missing_error = source_path.path() / "missing-error.txt";
    const auto unknown = executable + " clone '" + missing.url() + "' " +
                         shell_quote(missing_target) + " 2> " + shell_quote(missing_error);
    EXPECT_NE(std::system(unknown.c_str()), 0);
    error_input = std::ifstream{missing_error};
    error_text = std::string{std::istreambuf_iterator<char>{error_input}, {}};
    EXPECT_NE(error_text.find("not_found: repository does not exist"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(missing_target));
}
#endif

} // namespace
