#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace liquid::test {

namespace detail {

inline auto process_id() noexcept -> std::uint64_t {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class process_fixture_directory final {
public:
    process_fixture_directory() : process_fixture_directory(temporary_root()) {}

    explicit process_fixture_directory(const std::filesystem::path& temporary_root) {
        auto code = std::error_code{};
        auto random = std::random_device{};
        for (auto attempt = std::uint32_t{}; attempt < 128; ++attempt) {
            path_ = temporary_root / ("lq-" + std::to_string(process_id()) + "-" +
                                      std::to_string(random()) + "-" + std::to_string(attempt));
            code.clear();
            if (std::filesystem::create_directory(path_, code)) {
                return;
            }
            if (!code || code == std::errc::file_exists) {
                continue;
            }
            throw std::runtime_error(
                "liquid tests require a writable temporary root; cannot create "
                "a fixture directory under '" +
                temporary_root.string() + "': " + code.message());
        }

        throw std::runtime_error(
            "liquid tests could not create a unique fixture directory under '" +
            temporary_root.string() + "'");
    }

    ~process_fixture_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    process_fixture_directory(const process_fixture_directory&) = delete;
    auto operator=(const process_fixture_directory&) -> process_fixture_directory& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
        return path_;
    }

private:
    static auto temporary_root() -> std::filesystem::path {
        auto code = std::error_code{};
        auto temporary_root = std::filesystem::temp_directory_path(code);
        if (code) {
            throw std::runtime_error("liquid tests require a writable temporary root: " +
                                     code.message());
        }
        if (temporary_root.is_relative()) {
            temporary_root = std::filesystem::absolute(temporary_root, code);
            if (code) {
                throw std::runtime_error("liquid tests require an absolute temporary root: " +
                                         code.message());
            }
        }
        return temporary_root;
    }

    std::filesystem::path path_;
};

} // namespace detail

inline auto process_fixture_root() -> const std::filesystem::path& {
    static auto directory = detail::process_fixture_directory{};
    return directory.path();
}

inline auto fixture_path(const std::filesystem::path& name) -> std::filesystem::path {
    return process_fixture_root() / name;
}

} // namespace liquid::test
