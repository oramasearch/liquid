#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto contents(std::size_t changed_line) -> std::string {
    auto output = std::string{};
    output.reserve(160'000);
    for (auto line = std::size_t{1}; line <= 10'000; ++line) {
        output += line == changed_line ? "changed-line" : "line-" + std::to_string(line);
        output += '\n';
    }
    return output;
}

auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        return 2;
    }
    const auto path = std::filesystem::path{argv[1]};
    auto cleanup = std::error_code{};
    std::filesystem::remove_all(path, cleanup);
    auto created = liquid::runtime::block_on(liquid::repository::create(path));
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto repository = std::move(*created);
    auto initial_write =
        liquid::runtime::block_on(repository.write("large.txt", bytes(contents(0))));
    auto initial = initial_write
                       ? liquid::runtime::block_on(repository.cut("initial"))
                       : liquid::result<liquid::change_id>{std::unexpected(initial_write.error())};
    if (!initial) {
        std::cerr << initial.error().message << '\n';
        return 1;
    }
    auto changed_write =
        liquid::runtime::block_on(repository.write("large.txt", bytes(contents(5'001))));
    auto changed = changed_write
                       ? liquid::runtime::block_on(repository.cut("one line"))
                       : liquid::result<liquid::change_id>{std::unexpected(changed_write.error())};
    if (!changed) {
        std::cerr << changed.error().message << '\n';
        return 1;
    }

    auto samples = std::array<double, 5>{};
    for (auto& sample : samples) {
        const auto start = clock_type::now();
        auto attribution = liquid::runtime::block_on(repository.attribution("large.txt", *changed));
        const auto end = clock_type::now();
        if (!attribution || attribution->size() != 10'000) {
            std::cerr << (attribution ? "wrong line count" : attribution.error().message) << '\n';
            return 1;
        }
        sample = milliseconds(end - start);
    }
    std::ranges::sort(samples);
    auto load_average = std::array<double, 3>{};
    const auto load_count = getloadavg(load_average.data(), static_cast<int>(load_average.size()));
    std::cout << std::fixed << std::setprecision(3) << "lines=10000\n"
              << "samples_ms=";
    for (auto index = std::size_t{}; index < samples.size(); ++index) {
        std::cout << (index == 0 ? "" : ",") << samples[index];
    }
    std::cout << "\nmedian_ms=" << samples[samples.size() / 2] << '\n';
    if (load_count > 0) {
        std::cout << "load_average=" << load_average[0] << ',' << load_average[1] << ','
                  << load_average[2] << '\n';
    }
    std::filesystem::remove_all(path, cleanup);
    return 0;
}
