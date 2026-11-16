// SPDX-License-Identifier: MIT

#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include "liquid/node_store.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto cli_error(liquid::errc code, std::string message, std::string context = {}) -> liquid::error {
    return {code, std::move(message), std::move(context)};
}

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto environment(const char* name) -> std::string {
    const auto* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
}

auto parse_bytes(std::string_view text, std::string_view name) -> liquid::result<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "byte count is invalid", std::string{name}));
    }
    return output;
}

auto json_escape(std::string_view input) -> std::string {
    auto output = std::string{};
    for (const auto character : input) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    return output;
}

auto wire_request(std::string_view method, std::string_view target, std::string body)
    -> std::string {
    return std::format("{} {} HTTP/1.1\r\nHost: liquid-node-store\r\n"
                       "Content-Type: application/json\r\nContent-Length: {}\r\n"
                       "Connection: close\r\n\r\n{}",
                       method,
                       target,
                       body.size(),
                       body);
}

auto response_body(std::string response) -> liquid::result<std::string> {
    const auto first_space = response.find(' ');
    const auto second_space =
        first_space == std::string::npos ? first_space : response.find(' ', first_space + 1U);
    const auto header_end = response.find("\r\n\r\n");
    auto status = unsigned{};
    if (first_space == std::string::npos || second_space == std::string::npos ||
        header_end == std::string::npos) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "node-store returned an invalid HTTP response"));
    }
    const auto parsed =
        std::from_chars(response.data() + first_space + 1U, response.data() + second_space, status);
    if (parsed.ec != std::errc{} || parsed.ptr != response.data() + second_space) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "node-store returned an invalid HTTP status"));
    }
    auto body = response.substr(header_end + 4U);
    if (status < 200U || status >= 300U) {
        return std::unexpected(
            liquid::error{liquid::errc::unavailable, "node-store request failed", std::move(body)});
    }
    return body;
}

template <typename Socket>
auto exchange(Socket& socket, std::string request) -> liquid::runtime::task<std::string> {
    const auto [write_error, written] = co_await boost::capy::write(
        socket, boost::capy::const_buffer(request.data(), request.size()));
    if (write_error || written != request.size()) {
        co_return std::unexpected(
            cli_error(liquid::errc::io,
                      write_error ? write_error.message() : "short node-store HTTP request"));
    }
    auto response = std::string{};
    auto buffer = std::array<char, 4096>{};
    for (;;) {
        const auto [read_error, count] =
            co_await socket.read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
        if (count != 0) {
            response.append(buffer.data(), count);
        }
        if (read_error == boost::capy::error::eof || count == 0) {
            break;
        }
        if (read_error) {
            co_return std::unexpected(
                cli_error(liquid::errc::io, read_error.message(), "node-store HTTP response"));
        }
    }
    co_return response_body(std::move(response));
}

auto unix_request(std::filesystem::path socket_path,
                  std::string_view method,
                  std::string_view target,
                  std::string body = {}) -> liquid::runtime::task<std::string> {
#if defined(_WIN32)
    static_cast<void>(socket_path);
    static_cast<void>(method);
    static_cast<void>(target);
    static_cast<void>(body);
    co_return std::unexpected(
        cli_error(liquid::errc::unsupported, "Unix sockets are unavailable on Windows"));
#else
    const auto executor = co_await boost::capy::this_coro::executor;
    auto socket = boost::corosio::local_stream_socket{executor.context()};
    socket.open();
    const auto [connect_error] =
        co_await socket.connect(boost::corosio::local_endpoint{socket_path.string()});
    if (connect_error) {
        co_return std::unexpected(
            cli_error(liquid::errc::unavailable, connect_error.message(), socket_path.string()));
    }
    co_return co_await exchange(socket, wire_request(method, target, std::move(body)));
#endif
}

auto tcp_request(std::string address,
                 std::string_view method,
                 std::string_view target,
                 std::string body = {}) -> liquid::runtime::task<std::string> {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(address, endpoint)) {
        co_return std::unexpected(
            cli_error(liquid::errc::invalid_argument, code.message(), std::move(address)));
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    auto socket = boost::corosio::tcp_socket{executor.context()};
    const auto [connect_error] = co_await socket.connect(endpoint);
    if (connect_error) {
        co_return std::unexpected(
            cli_error(liquid::errc::unavailable, connect_error.message(), std::move(address)));
    }
    co_return co_await exchange(socket, wire_request(method, target, std::move(body)));
}

auto run_service(liquid::runtime::task<void> task) -> liquid::result<void> {
    auto mutex = std::mutex{};
    auto condition = std::condition_variable{};
    auto outcome = std::optional<liquid::result<void>>{};
    liquid::runtime::spawn(std::move(task), [&](liquid::result<void> value) {
        {
            auto lock = std::scoped_lock{mutex};
            outcome.emplace(std::move(value));
        }
        condition.notify_one();
    });
    auto lock = std::unique_lock{mutex};
    condition.wait(lock, [&] { return outcome.has_value(); });
    return std::move(*outcome);
}

auto default_socket() -> std::filesystem::path {
    auto value = environment("LIQUID_NODE_STORE_LISTEN");
    if (value.empty()) {
        return "/run/liquid/node-store.sock";
    }
    constexpr auto prefix = std::string_view{"unix://"};
    if (std::string_view{value}.starts_with(prefix)) {
        value.erase(0, prefix.size());
    }
    return value;
}

auto serve(int argc, char** argv) -> int {
    auto options = liquid::node_store_options{};
    options.root = environment("LIQUID_NODE_STORE_ROOT");
    if (const auto value = environment("LIQUID_NODE_STORE_LISTEN"); !value.empty()) {
        options.listen = value;
    }
    if (const auto value = environment("LIQUID_CONTROL_LISTEN"); !value.empty()) {
        options.control_listen = value;
    }
    if (const auto value = environment("LIQUID_NODE_STORE_BUDGET_BYTES"); !value.empty()) {
        auto parsed = parse_bytes(value, "LIQUID_NODE_STORE_BUDGET_BYTES");
        if (!parsed) {
            return usage(parsed.error().message);
        }
        options.budget_bytes = *parsed;
    }
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--root" && ++index < argc) {
            options.root = argv[index];
        } else if (argument == "--listen" && ++index < argc) {
            options.listen = argv[index];
        } else if (argument == "--control-listen" && ++index < argc) {
            options.control_listen = argv[index];
        } else if (argument == "--budget-bytes" && ++index < argc) {
            auto parsed = parse_bytes(argv[index], "--budget-bytes");
            if (!parsed) {
                return usage(parsed.error().message);
            }
            options.budget_bytes = *parsed;
        } else {
            return usage("node-store serve --root <path> [--listen <unix://path>] "
                         "[--control-listen <address>] [--budget-bytes <count>]");
        }
    }
    if (options.root.empty()) {
        return usage("node-store serve requires --root or LIQUID_NODE_STORE_ROOT");
    }
    std::println("node store listening on {}", options.listen);
    auto served = run_service(liquid::serve_node_store(std::move(options)));
    return served ? 0 : fail(served.error());
}

auto status(int argc, char** argv) -> int {
    auto control = environment("LIQUID_CONTROL_LISTEN");
    if (control.empty()) {
        control = "127.0.0.1:8082";
    }
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--control" && ++index < argc) {
            control = argv[index];
        } else if (argument != "--json") {
            return usage("node-store status [--control <address>] [--json]");
        }
    }
    auto response = liquid::runtime::block_on(tcp_request(control, "GET", "/v1"));
    if (!response) {
        return fail(response.error());
    }
    std::print("{}", *response);
    return 0;
}

auto socket_argument(int& index, int argc, char** argv, std::filesystem::path& socket_path)
    -> bool {
    if (std::string_view{argv[index]} != "--socket" || ++index >= argc) {
        return false;
    }
    socket_path = argv[index];
    return true;
}

auto mount(int argc, char** argv) -> int {
    if (argc < 3) {
        return usage("node-store mount <tree> --store <url> [--socket <path>]");
    }
    auto parsed = liquid::parse_digest(argv[0]);
    if (!parsed) {
        return usage("node-store mount requires a valid tree digest");
    }
    auto store_url = std::string{};
    auto socket_path = default_socket();
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--store" && ++index < argc) {
            store_url = argv[index];
        } else if (!socket_argument(index, argc, argv, socket_path)) {
            return usage("node-store mount <tree> --store <url> [--socket <path>]");
        }
    }
    if (store_url.empty()) {
        return usage("node-store mount requires --store <url>");
    }
    const auto body = std::format(R"({{"tree":"{}","store_url":"{}","bearer":"{}"}})",
                                  liquid::to_string(*parsed),
                                  json_escape(store_url),
                                  json_escape(environment("LIQUID_TOKEN")));
    auto response = liquid::runtime::block_on(unix_request(socket_path, "POST", "/mount", body));
    if (!response) {
        return fail(response.error());
    }
    std::print("{}", *response);
    return 0;
}

auto tree_request(int argc, char** argv, std::string_view target) -> int {
    if (argc < 1) {
        return usage("node-store command requires a tree digest");
    }
    auto parsed = liquid::parse_digest(argv[0]);
    if (!parsed) {
        return usage("node-store command requires a valid tree digest");
    }
    auto socket_path = default_socket();
    for (auto index = 1; index < argc; ++index) {
        if (!socket_argument(index, argc, argv, socket_path)) {
            return usage("node-store command accepts only --socket <path>");
        }
    }
    const auto body = std::format(R"({{"tree":"{}"}})", liquid::to_string(*parsed));
    auto response = liquid::runtime::block_on(unix_request(socket_path, "POST", target, body));
    if (!response) {
        return fail(response.error());
    }
    std::print("{}", *response);
    return 0;
}

} // namespace

auto node_store(int argc, char** argv) -> int {
    if (argc == 0) {
        return usage("node-store requires serve, status, mount, unmount, or prefetch");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "serve") {
        return serve(argc - 1, argv + 1);
    }
    if (command == "status") {
        return status(argc - 1, argv + 1);
    }
    if (command == "mount") {
        return mount(argc - 1, argv + 1);
    }
    if (command == "unmount") {
        return tree_request(argc - 1, argv + 1, "/unmount");
    }
    if (command == "prefetch") {
        return tree_request(argc - 1, argv + 1, "/prefetch");
    }
    return usage("node-store requires serve, status, mount, unmount, or prefetch");
}

} // namespace liquid_cli
