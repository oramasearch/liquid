// SPDX-License-Identifier: MIT

#include "crdt/document.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

auto identifier(std::uint64_t value, std::byte prefix) -> liquid::crdt::node_id {
    auto result = liquid::crdt::node_id{};
    result[0] = prefix;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        result[result.size() - 1 - i] = std::byte{static_cast<unsigned char>(value >> (i * 8U))};
    }
    return result;
}

auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

auto peak_rss_mib() -> double {
    auto usage = rusage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0.0;
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

} // namespace

auto main(int argc, char **argv) -> int {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: liquid-crdt-measure ENTRIES [BATCH_SIZE]\n";
        return 2;
    }
    const auto entries = std::stoull(argv[1]);
    const auto batch_size = argc == 3 ? std::stoull(argv[2]) : std::uint64_t{1000};
    if (entries == 0 || batch_size == 0)
        return 2;

    auto repository = liquid::crdt::repo_id{};
    repository[0] = std::byte{0x55};
    auto created = liquid::crdt::document::create(repository, "taap:measure", "device");
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto document = std::move(*created);
    const auto transaction_start = clock_type::now();
    for (std::uint64_t offset = 0; offset < entries; offset += batch_size) {
        const auto count = std::min(batch_size, entries - offset);
        auto operations = std::vector<liquid::crdt::operation>{};
        operations.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto index = offset + i + 1;
            operations.push_back(liquid::crdt::operation{
                .id = identifier(index, std::byte{0xa1}),
                .kind = liquid::crdt::operation_kind::write,
                .path = "entry-" + std::to_string(index),
                .ref = "blake3:"
                       "0000000000000000000000000000000000000000000000000000000000000"
                       "000",
                .mode = 0100644,
                .entry_id = identifier(index, std::byte{0xb2}),
            });
        }
        auto applied = document.transact(std::move(operations));
        if (!applied) {
            std::cerr << applied.error().message << ": " << applied.error().context << '\n';
            return 1;
        }
        if ((offset + count) % 100000 == 0 || offset + count == entries)
            std::cerr << "entries=" << offset + count << '\n';
    }
    const auto transaction_end = clock_type::now();

    const auto save_start = clock_type::now();
    auto saved = document.save();
    const auto save_end = clock_type::now();
    if (!saved) {
        std::cerr << saved.error().message << '\n';
        return 1;
    }

    const auto load_start = clock_type::now();
    auto loaded = liquid::crdt::document::load(*saved, "taap:measure", "device");
    const auto load_end = clock_type::now();
    if (!loaded) {
        std::cerr << loaded.error().message << '\n';
        return 1;
    }
    auto last_entry = loaded->inspect("entry-" + std::to_string(entries));
    if (!last_entry || !*last_entry) {
        std::cerr << "loaded document is missing the final entry\n";
        return 1;
    }

    const auto transaction_ms = milliseconds(transaction_end - transaction_start);
    std::cout << std::fixed << std::setprecision(3) << "entries=" << entries << '\n'
              << "batch_size=" << batch_size << '\n'
              << "transaction_ms=" << transaction_ms << '\n'
              << "transaction_us_per_write=" << (transaction_ms * 1000.0 / entries) << '\n'
              << "save_bytes=" << saved->size() << '\n'
              << "save_ms=" << milliseconds(save_end - save_start) << '\n'
              << "load_ms=" << milliseconds(load_end - load_start) << '\n'
              << "peak_rss_mib=" << peak_rss_mib() << '\n';
    return 0;
}
