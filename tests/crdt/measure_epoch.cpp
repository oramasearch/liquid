// SPDX-License-Identifier: MIT

#include "crdt/document.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

auto identifier(std::uint64_t value, std::byte prefix) -> liquid::crdt::node_id {
    auto result = liquid::crdt::node_id{};
    result[0] = prefix;
    for (std::size_t index = 0; index != sizeof(value); ++index) {
        result[result.size() - 1 - index] =
            std::byte{static_cast<unsigned char>(value >> (index * 8U))};
    }
    return result;
}

auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

auto reference(std::uint64_t value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{"blake3:"} + std::string(64, '0');
    output.back() = digits[value % digits.size()];
    return output;
}

} // namespace

auto main(int argc, char** argv) -> int {
    const auto writes = argc > 1 ? std::stoull(argv[1]) : std::uint64_t{100'000};
    const auto paths = argc > 2 ? std::stoull(argv[2]) : std::uint64_t{100};
    const auto batch_size = argc > 3 ? std::stoull(argv[3]) : std::uint64_t{1'000};
    if (writes == 0 || paths == 0 || batch_size == 0) {
        return 2;
    }
    auto repository = liquid::crdt::repo_id{};
    repository[0] = std::byte{0x30};
    auto created = liquid::crdt::document::create(repository, "taap:epoch-measure", "agent");
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto document = std::move(*created);
    for (std::uint64_t offset = 0; offset < writes; offset += batch_size) {
        const auto count = std::min(batch_size, writes - offset);
        auto operations = std::vector<liquid::crdt::operation>{};
        operations.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index != count; ++index) {
            const auto operation = offset + index;
            const auto path = operation % paths;
            operations.push_back({.id = identifier(operation + 1, std::byte{0xa1}),
                                  .kind = liquid::crdt::operation_kind::write,
                                  .path = "agent-output-" + std::to_string(path),
                                  .ref = reference(operation),
                                  .mode = 0100644,
                                  .entry_id = identifier(path + 1, std::byte{0xb2})});
        }
        auto applied = document.transact(std::move(operations));
        if (!applied) {
            std::cerr << applied.error().message << ": " << applied.error().context << '\n';
            return 1;
        }
    }
    auto before = document.save();
    if (!before) {
        std::cerr << before.error().message << '\n';
        return 1;
    }
    const auto before_load_start = clock_type::now();
    auto before_loaded = liquid::crdt::document::load(*before, "taap:epoch-measure", "reader");
    const auto before_load_end = clock_type::now();
    if (!before_loaded) {
        std::cerr << before_loaded.error().message << '\n';
        return 1;
    }
    auto compact = document.save_compact();
    if (!compact) {
        std::cerr << compact.error().message << '\n';
        return 1;
    }
    const auto after_load_start = clock_type::now();
    auto after_loaded =
        liquid::crdt::document::load_shallow(*compact, "taap:epoch-measure", "reader");
    const auto after_load_end = clock_type::now();
    if (!after_loaded) {
        std::cerr << after_loaded.error().message << '\n';
        return 1;
    }
    auto load_average = std::array<double, 3>{};
    const auto load_count = getloadavg(load_average.data(), static_cast<int>(load_average.size()));
    std::cout << std::fixed << std::setprecision(3) << "writes=" << writes << '\n'
              << "rewrite_paths=" << paths << '\n'
              << "before_bytes=" << before->size() << '\n'
              << "before_load_ms=" << milliseconds(before_load_end - before_load_start) << '\n'
              << "after_bytes=" << compact->size() << '\n'
              << "after_load_ms=" << milliseconds(after_load_end - after_load_start) << '\n';
    if (load_count > 0) {
        std::cout << "load_average=" << load_average[0] << ',' << load_average[1] << ','
                  << load_average[2] << '\n';
    }
    return 0;
}
