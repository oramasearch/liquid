// SPDX-License-Identifier: MIT

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

auto peak_rss_mib() -> double {
    auto usage = rusage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

auto configure_identity_home(const std::filesystem::path& root) -> bool {
    const auto identity_home = root / ".liquid-home";
#if defined(_WIN32)
    return _putenv_s("LIQUID_HOME", identity_home.string().c_str()) == 0;
#else
    return setenv("LIQUID_HOME", identity_home.string().c_str(), 1) == 0;
#endif
}

auto path_for(std::uint64_t index, std::uint64_t directories) -> std::string {
    return "directory-" + std::to_string(index % directories) + "/value.txt";
}

auto prepare(const std::filesystem::path& root, std::uint64_t directories, std::uint64_t drips)
    -> int {
    auto options = liquid::repository_options{};
    options.checkpoint_interval = 64;
    auto created = liquid::runtime::block_on(liquid::repository::create(root, options));
    if (!created) {
        std::cerr << created.error().message << ": " << created.error().context << '\n';
        return 1;
    }
    auto repository = std::move(*created);
    for (std::uint64_t index = 0; index < drips; ++index) {
        const auto value = std::to_string(index);
        auto written =
            liquid::runtime::block_on(repository.write(path_for(index, directories), bytes(value)));
        if (!written) {
            std::cerr << written.error().message << ": " << written.error().context << '\n';
            return 1;
        }
        if ((index + 1) % 1000 == 0 || index + 1 == drips) {
            std::cerr << "drips=" << index + 1 << '\n';
        }
    }
    std::cout << "prepared_directories=" << directories << '\n';
    std::cout << "prepared_drips=" << drips << '\n';
    return 0;
}

auto measure_open(const std::filesystem::path& root) -> int {
    auto options = liquid::repository_options{};
    options.checkpoint_interval = 64;
    const auto start = clock_type::now();
    auto opened = liquid::runtime::block_on(liquid::repository::open(root, options));
    const auto finish = clock_type::now();
    if (!opened) {
        std::cerr << opened.error().message << ": " << opened.error().context << '\n';
        return 1;
    }
    auto status = opened->status();
    if (!status) {
        std::cerr << status.error().message << ": " << status.error().context << '\n';
        return 1;
    }
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "open_ms=" << milliseconds(finish - start) << '\n';
    std::cout << "peak_rss_mib=" << peak_rss_mib() << '\n';
    std::cout << "pending_drips=" << status->pending_drips << '\n';
    return 0;
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: liquid-repository-open-measure prepare ROOT [DIRECTORIES DRIPS]\n"
                     "       liquid-repository-open-measure open ROOT\n";
        return 2;
    }
    const auto mode = std::string_view{argv[1]};
    const auto root = std::filesystem::path{argv[2]};
    if (!configure_identity_home(root)) {
        std::cerr << "cannot configure LIQUID_HOME\n";
        return 1;
    }
    if (mode == "prepare") {
        const auto directories = argc >= 4 ? std::stoull(argv[3]) : std::uint64_t{100};
        const auto drips = argc >= 5 ? std::stoull(argv[4]) : std::uint64_t{10000};
        if (directories == 0 || drips == 0) {
            return 2;
        }
        return prepare(root, directories, drips);
    }
    if (mode == "open" && argc == 3) {
        return measure_open(root);
    }
    return 2;
}
