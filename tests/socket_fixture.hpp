// SPDX-License-Identifier: MIT

#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/un.h>
#include <system_error>

namespace liquid::test {

class socket_fixture final {
public:
    socket_fixture() : socket_fixture(std::filesystem::temp_directory_path()) {}

    explicit socket_fixture(const std::filesystem::path& temporary_root) {
        auto pattern = temporary_root / "lq-sock-XXXXXX";
        if (!pattern.is_absolute() ||
            (pattern / "socket").native().size() >= sizeof(sockaddr_un::sun_path)) {
            pattern = "/tmp/lq-sock-XXXXXX";
        }
        auto text = pattern.string();
        if (::mkdtemp(text.data()) == nullptr) {
            throw std::runtime_error("cannot create socket fixture directory '" + text +
                                     "': " + std::strerror(errno));
        }
        directory_ = text;
    }

    ~socket_fixture() {
        auto code = std::error_code{};
        std::filesystem::remove_all(directory_, code);
    }

    socket_fixture(const socket_fixture&) = delete;
    auto operator=(const socket_fixture&) -> socket_fixture& = delete;

    [[nodiscard]] auto path() const -> std::filesystem::path {
        return directory_ / "socket";
    }

private:
    std::filesystem::path directory_;
};

} // namespace liquid::test
