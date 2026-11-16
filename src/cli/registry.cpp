// SPDX-License-Identifier: MIT

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <print>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/tuf.hpp"

namespace liquid_cli {
namespace {

constexpr std::uintmax_t maximum_root_size = 16U * 1024U * 1024U;

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: registry trust <pin <root.json>|show>");
    return 2;
}

auto read_root(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    auto code = std::error_code{};
    const auto size = std::filesystem::file_size(path, code);
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), path.string()});
    }
    if (size == 0 || size > maximum_root_size) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "root file has an invalid size", path.string()});
    }
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "could not open root file", path.string()});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "could not read complete root file", path.string()});
    }
    return output;
}

auto print_root(const liquid::tuf::trusted_root& root, bool include_encoded) -> void {
    std::println("version={}", root.version);
    std::println("sha256={}", liquid::to_string(root.sha256));
    if (include_encoded) {
        std::println("root={}",
                     std::string_view{reinterpret_cast<const char*>(root.encoded.data()),
                                      root.encoded.size()});
    }
}

} // namespace

auto registry(int argc, char** argv) -> int {
    if (argc < 2 || std::string_view{argv[0]} != "trust") {
        return usage();
    }
    auto subject =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!subject) {
        return fail(subject.error());
    }
    const auto command = std::string_view{argv[1]};
    if (command == "pin" && argc == 3) {
        auto encoded = read_root(argv[2]);
        if (!encoded) {
            return fail(encoded.error());
        }
        auto pinned =
            liquid::runtime::block_on(liquid::tuf::client::pin(*subject, std::move(*encoded)));
        if (!pinned) {
            return fail(pinned.error());
        }
        print_root(*pinned, false);
        return 0;
    }
    if (command == "show" && argc == 2) {
        auto trusted = liquid::runtime::block_on(liquid::tuf::client::trusted(*subject));
        if (!trusted) {
            return fail(trusted.error());
        }
        print_root(*trusted, true);
        return 0;
    }
    return usage();
}

} // namespace liquid_cli
