#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"
#include "sync/codec.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("liquid-sync-test-" + std::to_string(::getpid()) + "-" + std::to_string(next_++))) {
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

class sync_server_double {
public:
    sync_server_double() {
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
        auto initial = liquid::sync_detail::encode_metadata({});
        if (!initial) {
            ::close(listener_);
            throw std::runtime_error(initial.error().message);
        }
        metadata_ = std::move(*initial);
        thread_ = std::thread([this] { serve(); });
    }

    ~sync_server_double() {
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

    auto object_count() const -> std::size_t {
        auto lock = std::lock_guard{mutex_};
        return objects_.size();
    }

    auto uploaded_bytes() const -> std::uint64_t {
        auto lock = std::lock_guard{mutex_};
        return uploaded_bytes_;
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
            respond(connection, 200, metadata_);
        } else if (method == "GET" && target.find("/sync/chunks/") != std::string_view::npos) {
            read_range(connection, target);
        } else {
            respond(connection, 404, {});
        }
    }

    void find_missing(int connection, std::span<const std::byte> body) {
        auto requested = liquid::sync_detail::decode_object_list(body);
        if (!requested) {
            respond(connection, 400, {});
            return;
        }
        auto missing = std::vector<liquid::digest>{};
        {
            auto lock = std::lock_guard{mutex_};
            for (const auto& id : *requested) {
                if (!objects_.contains(liquid::to_string(id))) {
                    missing.push_back(id);
                }
            }
        }
        auto encoded = liquid::sync_detail::encode_object_list(missing);
        respond(connection, encoded ? 200 : 500, encoded ? *encoded : std::vector<std::byte>{});
    }

    void store_chunks(int connection, std::span<const std::byte> body) {
        auto frames = liquid::sync_detail::decode_pack_frames(body);
        if (!frames) {
            respond(connection, 400, {});
            return;
        }
        {
            auto lock = std::lock_guard{mutex_};
            for (auto& [id, value] : *frames) {
                uploaded_bytes_ += value.size();
                objects_[liquid::to_string(id)] = std::move(value);
            }
        }
        respond(connection, 200, {});
    }

    void push(int connection, std::span<const std::byte> body) {
        auto decoded = liquid::sync_detail::decode_metadata(body);
        if (!decoded) {
            respond(connection, 400, {});
            return;
        }
        metadata_.assign(body.begin(), body.end());
        auto reply = liquid::sync_detail::encode_metadata(
            liquid::sync_detail::metadata_document{.cursor = decoded->cursor});
        respond(connection, reply ? 200 : 500, reply ? *reply : std::vector<std::byte>{});
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
        auto lock = std::lock_guard{mutex_};
        const auto found = objects_.find(liquid::to_string(*id));
        if (found == objects_.end()) {
            respond(connection, 404, {});
            return;
        }
        const auto start = std::min<std::uint64_t>(offset, found->second.size());
        const auto count = std::min<std::uint64_t>(length, found->second.size() - start);
        respond(connection, 200, std::span<const std::byte>{found->second}.subspan(start, count));
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
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::byte>> objects_;
    std::vector<std::byte> metadata_;
    std::uint64_t uploaded_bytes_{};
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
    sync_server_double server;
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
    EXPECT_EQ(first->sent.bytes, server.uploaded_bytes());

    const auto objects_before = server.object_count();
    const auto bytes_before = server.uploaded_bytes();
    ASSERT_TRUE(liquid::runtime::block_on(source->write("two.txt", bytes("two"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));
    auto second = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second) << second.error().message << " " << second.error().context;
    EXPECT_EQ(second->sent.objects, server.object_count() - objects_before);
    EXPECT_EQ(second->sent.bytes, server.uploaded_bytes() - bytes_before);
    EXPECT_GT(second->sent.objects, 0U);

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

TEST(Sync, cli_push_and_pull_round_trip) {
    temporary_directory source_path;
    temporary_directory destination_path;
    sync_server_double server;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_path.path()));
    ASSERT_TRUE(source);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(liquid::runtime::block_on(source->write("cli.txt", bytes("round trip"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("cli")));
    *source = liquid::repository{};
    *destination = liquid::repository{};
    const auto executable = shell_quote(LIQUID_CLI);
    const auto push = "cd " + shell_quote(source_path.path()) + " && " + executable + " push '" +
                      server.url() + "'";
    const auto pull = "cd " + shell_quote(destination_path.path()) + " && " + executable +
                      " pull '" + server.url() + "'";
    ASSERT_EQ(std::system(push.c_str()), 0);
    ASSERT_EQ(std::system(pull.c_str()), 0);
    auto opened = liquid::runtime::block_on(liquid::repository::open(destination_path.path()));
    ASSERT_TRUE(opened);
    auto content = liquid::runtime::block_on(opened->read("cli.txt"));
    ASSERT_TRUE(content) << content.error().message << " " << content.error().context;
    EXPECT_EQ(text(*content), "round trip");
}
#endif

} // namespace
