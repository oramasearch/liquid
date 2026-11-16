// SPDX-License-Identifier: MIT

#include <cstddef>
#include <iostream>
#include <vector>

#include <liquid/repository.hpp>
#include <liquid/runtime.hpp>

namespace {

const std::vector<std::byte> contents{
    std::byte{'l'}, std::byte{'i'}, std::byte{'q'}, std::byte{'u'}, std::byte{'i'}, std::byte{'d'}};

liquid::runtime::task<void> check_contents(liquid::repository& repository) {
    co_await liquid::runtime::yield();
    auto read = co_await repository.read("hello.txt");
    if (!read) {
        co_return std::unexpected(read.error());
    }
    if (*read != contents) {
        co_return std::unexpected(liquid::error{
            liquid::errc::corrupt, "file contents did not survive write/reopen", "consumer"});
    }
    co_return {};
}

liquid::runtime::task<void> create_repository() {
    auto repository = co_await liquid::repository::create(".");
    if (!repository) {
        co_return std::unexpected(repository.error());
    }
    auto written = co_await repository->write("hello.txt", contents);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return co_await check_contents(*repository);
}

int report_error(const liquid::error& error) {
    std::cerr << error.context << ": " << error.message << '\n';
    return 1;
}

} // namespace

int main() {
    const auto created = liquid::runtime::block_on(create_repository());
    if (!created) {
        return report_error(created.error());
    }
    std::cout << "repository create, file write/read: passed\n";

    auto repo = liquid::runtime::block_on(liquid::repository::open("."));
    if (!repo) {
        return report_error(repo.error());
    }
    const auto read = liquid::runtime::block_on(check_contents(*repo));
    if (!read) {
        return report_error(read.error());
    }
    std::cout << "repository reopen, persisted file read, coroutine yield: passed\n";
    return 0;
}
