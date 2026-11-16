// SPDX-License-Identifier: MIT

// A store writer process for the liquid #321 owner-lock tests.
//
//   liquid-store-owner-fixture <root> <ack-file> <seed> <count> <owner-wait-ms>
//
// Opens the store at <root>, waiting up to <owner-wait-ms> for its current owner, then writes
// <count> objects (0 means until killed). After each put returns, it appends "<index> <digest>"
// to <ack-file>: a line in that file is an acknowledged write. Object bytes are a pure function
// of (seed, index) so a verifier can regenerate them. Exit 3 means the open was refused because
// another owner holds the root; the refusal message goes to stderr.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "owner_fixture_content.hpp"

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fputs("usage: liquid-store-owner-fixture <root> <ack> <seed> <count> <wait-ms>\n",
                   stderr);
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path ack = argv[2];
    const auto seed = std::strtoull(argv[3], nullptr, 10);
    const auto count = std::strtoull(argv[4], nullptr, 10);
    const auto wait = std::chrono::milliseconds{std::strtoll(argv[5], nullptr, 10)};

    liquid::store_options options;
    options.exclusive_owner = true;
    options.owner_wait = wait;
    auto opened = liquid::runtime::block_on(liquid::store::open(root, options));
    if (!opened) {
        std::fprintf(stderr,
                     "open: %s (%s)\n",
                     opened.error().message.c_str(),
                     opened.error().context.c_str());
        return opened.error().code == liquid::errc::locked ? 3 : 1;
    }
    const auto descriptor = ::open(ack.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        std::perror("ack");
        return 1;
    }
    for (std::uint64_t index = 0; count == 0 || index < count; ++index) {
        auto written =
            liquid::runtime::block_on(opened->put(liquid::test::owner_fixture_bytes(seed, index)));
        if (!written) {
            std::fprintf(stderr,
                         "put %llu: %s\n",
                         static_cast<unsigned long long>(index),
                         written.error().message.c_str());
            return 1;
        }
        const auto line = std::to_string(index) + " " + liquid::to_string(*written) + "\n";
        if (::write(descriptor, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
            std::perror("ack write");
            return 1;
        }
    }
    ::close(descriptor);
    return 0;
}
