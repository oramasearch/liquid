// SPDX-License-Identifier: MIT

// Writer processes for the liquid #526 tests: several processes writing one store without an
// owner, the way a checkout's daemon, the CLI and an IDE do.
//
//   liquid-store-writers-fixture store <root> <ack> <seed> <count> <pack-size> <peers-dir>
//       Opens the store at <root> without exclusive ownership and writes <count> objects, leasing
//       each for an hour. After each put and lease it appends "<index> <digest>" to <ack>. Every
//       few writes it reads back one of its own objects and the newest object of every other
//       writer's ack file in <peers-dir>, and every 16 writes it closes and reopens the store.
//       Object bytes are owner_fixture_bytes(seed, index).
//   liquid-store-writers-fixture collect <root> <stop-file>
//       Opens the store, runs a full collection and closes it, until <stop-file> exists.
//   liquid-store-writers-fixture killed <checkout>
//       Opens the repository at <checkout>, writes one file, prints its pid and kills itself with
//       SIGKILL, leaving the leases a crashed handle leaves (liquid #527).
//   liquid-store-writers-fixture repository <checkout> <ack> <stop-file>
//       A library client: opens the repository at <checkout>, writes library/<n>.txt, appends
//       "<n> <digest>" to <ack> and closes it, until <stop-file> exists.
//
// Exit 0 is success; any failure prints the operation and the error and exits 1.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "owner_fixture_content.hpp"

namespace {

int failed(const char* operation, const liquid::error& value) {
    std::fprintf(stderr, "%s: %s (%s)\n", operation, value.message.c_str(), value.context.c_str());
    return 1;
}

bool append_line(const std::filesystem::path& ack, const std::string& line) {
    const auto descriptor = ::open(ack.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return false;
    const auto written = ::write(descriptor, line.data(), line.size());
    ::close(descriptor);
    return written == static_cast<ssize_t>(line.size());
}

// The last complete "<index> <digest>" line of an ack file.
std::optional<std::pair<std::uint64_t, liquid::digest>> newest(const std::filesystem::path& ack) {
    std::ifstream input(ack);
    std::optional<std::pair<std::uint64_t, liquid::digest>> output;
    std::string line;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::size_t start = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
        line = text.substr(start, end - start);
        start = end + 1;
        const auto space = line.find(' ');
        if (space == std::string::npos)
            continue;
        auto parsed = liquid::parse_digest(line.substr(space + 1));
        if (parsed)
            output = std::pair{std::stoull(line.substr(0, space)), *parsed};
    }
    return output;
}

int write_store(int argc, char** argv) {
    if (argc != 8) {
        std::fputs("usage: store <root> <ack> <seed> <count> <pack-size> <peers-dir>\n", stderr);
        return 2;
    }
    const std::filesystem::path root = argv[2];
    const std::filesystem::path ack = argv[3];
    const auto seed = std::strtoull(argv[4], nullptr, 10);
    const auto count = std::strtoull(argv[5], nullptr, 10);
    const std::filesystem::path peers = argv[7];
    liquid::store_options options;
    options.pack_size = std::strtoull(argv[6], nullptr, 10);

    auto opened = liquid::runtime::block_on(liquid::store::open(root, options));
    if (!opened)
        return failed("open", opened.error());
    auto handle = std::move(*opened);
    std::vector<liquid::lease> leases;
    std::vector<liquid::digest> written;
    for (std::uint64_t index = 0; index < count; ++index) {
        // A collection in another process may remove the object between the put and the lease,
        // because nothing retains it yet; a client then stores it again. An acknowledged object
        // is a leased one, and those must all survive.
        auto put =
            liquid::runtime::block_on(handle.put(liquid::test::owner_fixture_bytes(seed, index)));
        if (!put)
            return failed("put", put.error());
        auto leased = liquid::runtime::block_on(handle.lease_root(*put, std::chrono::hours{1}));
        for (auto attempt = 0;
             !leased && leased.error().code == liquid::errc::not_found && attempt != 8;
             ++attempt) {
            put = liquid::runtime::block_on(
                handle.put(liquid::test::owner_fixture_bytes(seed, index)));
            if (!put)
                return failed("put again", put.error());
            leased = liquid::runtime::block_on(handle.lease_root(*put, std::chrono::hours{1}));
        }
        if (!leased)
            return failed("lease", leased.error());
        // An object nothing retains, for the collector to remove.
        auto garbage = liquid::runtime::block_on(
            handle.put(liquid::test::owner_fixture_bytes(seed + 1000, index)));
        if (!garbage)
            return failed("put garbage", garbage.error());
        leases.push_back(std::move(*leased));
        written.push_back(*put);
        if (!append_line(ack, std::to_string(index) + " " + liquid::to_string(*put) + "\n")) {
            std::perror("ack");
            return 1;
        }
        if (index % 4 == 3) {
            const auto own = (index * 7) % written.size();
            auto read = liquid::runtime::block_on(handle.get(written[own]));
            if (!read)
                return failed("read own", read.error());
            if (*read != liquid::test::owner_fixture_bytes(seed, own)) {
                std::fprintf(stderr,
                             "read own: object %llu differs\n",
                             static_cast<unsigned long long>(own));
                return 1;
            }
            for (const auto& peer : std::filesystem::directory_iterator(peers)) {
                const auto name = peer.path().filename().string();
                if (!name.starts_with("ack-") || peer.path() == ack)
                    continue;
                const auto other = newest(peer.path());
                if (!other)
                    continue;
                auto other_read = liquid::runtime::block_on(handle.get(other->second));
                if (!other_read)
                    return failed("read peer", other_read.error());
                const auto peer_seed = std::strtoull(name.c_str() + 4, nullptr, 10);
                if (*other_read != liquid::test::owner_fixture_bytes(peer_seed, other->first)) {
                    std::fprintf(stderr,
                                 "read peer: %s object %llu differs\n",
                                 name.c_str(),
                                 static_cast<unsigned long long>(other->first));
                    return 1;
                }
            }
        }
        if (index % 16 == 15) {
            // The CLI and the daemon open a checkout's store once per operation; so does this.
            auto closed = liquid::runtime::block_on(handle.close());
            if (!closed)
                return failed("close", closed.error());
            auto reopened = liquid::runtime::block_on(liquid::store::open(root, options));
            if (!reopened)
                return failed("reopen", reopened.error());
            handle = std::move(*reopened);
        }
    }
    return 0;
}

int collect(int argc, char** argv) {
    if (argc != 4) {
        std::fputs("usage: collect <root> <stop-file>\n", stderr);
        return 2;
    }
    const std::filesystem::path root = argv[2];
    const std::filesystem::path stop = argv[3];
    std::uint64_t runs = 0;
    while (!std::filesystem::exists(stop)) {
        auto opened = liquid::runtime::block_on(liquid::store::open(root));
        if (!opened)
            return failed("open", opened.error());
        auto collected = liquid::runtime::block_on(opened->collect());
        if (!collected)
            return failed("collect", collected.error());
        ++runs;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    std::printf("collections=%llu\n", static_cast<unsigned long long>(runs));
    return 0;
}

int write_repository(int argc, char** argv) {
    if (argc != 5) {
        std::fputs("usage: repository <checkout> <ack> <stop-file>\n", stderr);
        return 2;
    }
    const std::filesystem::path checkout = argv[2];
    const std::filesystem::path ack = argv[3];
    const std::filesystem::path stop = argv[4];
    for (std::uint64_t index = 0; !std::filesystem::exists(stop); ++index) {
        auto opened = liquid::runtime::block_on(liquid::repository::open(checkout));
        if (!opened)
            return failed("open", opened.error());
        const auto text = "library write " + std::to_string(index) + "\n";
        auto bytes =
            std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                   reinterpret_cast<const std::byte*>(text.data()) + text.size());
        const auto content = liquid::hash(liquid::algorithm::blake3, bytes);
        if (!content)
            return failed("hash", content.error());
        auto written = liquid::runtime::block_on(
            opened->write("library/" + std::to_string(index % 8) + ".txt", std::move(bytes)));
        if (!written)
            return failed("write", written.error());
        if (!append_line(ack, std::to_string(index) + " " + liquid::to_string(*content) + "\n")) {
            std::perror("ack");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return 0;
}

int killed(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("usage: killed <checkout>\n", stderr);
        return 2;
    }
    auto opened = liquid::runtime::block_on(liquid::repository::open(argv[2]));
    if (!opened)
        return failed("open", opened.error());
    const auto text = std::string{"written before the crash\n"};
    auto written = liquid::runtime::block_on(opened->write(
        "crashed.txt",
        std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                               reinterpret_cast<const std::byte*>(text.data()) + text.size())));
    if (!written)
        return failed("write", written.error());
    std::printf("%ld\n", static_cast<long>(::getpid()));
    std::fflush(stdout);
    ::kill(::getpid(), SIGKILL);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const auto mode = argc >= 2 ? std::string{argv[1]} : std::string{};
    if (mode == "store")
        return write_store(argc, argv);
    if (mode == "collect")
        return collect(argc, argv);
    if (mode == "repository")
        return write_repository(argc, argv);
    if (mode == "killed")
        return killed(argc, argv);
    std::fputs("usage: liquid-store-writers-fixture <store|collect|repository|killed> ...\n",
               stderr);
    return 2;
}
